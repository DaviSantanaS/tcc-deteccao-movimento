#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

static inline int64_t fps_to_tb_step(const AVRational avg_fr, const AVRational tb) {
    // step = 1/fps em unidades do time_base: rescale(1, 1/fps -> tb)
    AVRational inv_fps = { avg_fr.den, avg_fr.num }; // 1/fps
    if (avg_fr.num == 0 || avg_fr.den == 0) { // fallback 25fps
        inv_fps = { 1, 25 };
    }
    return av_rescale_q(1, inv_fps, tb);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <rtsp_url> <output.mp4>\n", argv[0]);
        return 1;
    }
    const char* rtsp_url    = argv[1];
    const char* output_file = argv[2];

    av_log_set_level(AV_LOG_WARNING);
    avformat_network_init();

    AVFormatContext* in_ctx  = nullptr;
    AVFormatContext* out_ctx = nullptr;

    // (1) Abrir input RTSP com robustez mínima
    AVDictionary* in_opts = nullptr;
    av_dict_set(&in_opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&in_opts, "stimeout", "5000000", 0); // 5s
    av_dict_set(&in_opts, "max_delay", "500000", 0); // 0.5s
    if (avformat_open_input(&in_ctx, rtsp_url, nullptr, &in_opts) < 0) {
        std::fprintf(stderr, "Could not open input: %s\n", rtsp_url);
        av_dict_free(&in_opts);
        return 1;
    }
    av_dict_free(&in_opts);

    // peça ao demuxer para gerar PTS quando possível
    in_ctx->flags |= AVFMT_FLAG_GENPTS;

    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        std::fprintf(stderr, "Could not find stream info\n");
        avformat_close_input(&in_ctx);
        return 1;
    }

    // (2) Output .mp4
    if (avformat_alloc_output_context2(&out_ctx, nullptr, nullptr, output_file) < 0 || !out_ctx) {
        std::fprintf(stderr, "Could not create output context: %s\n", output_file);
        avformat_close_input(&in_ctx);
        return 1;
    }

    // (3) Copiar streams + manter time_base
    std::vector<int64_t> next_ts;      // próximo timestamp sintético por stream (no time_base do input)
    std::vector<int64_t> step_ts;      // passo de tempo por stream (no time_base do input)
    next_ts.resize(in_ctx->nb_streams, 0);
    step_ts.resize(in_ctx->nb_streams, 0);

    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        AVStream* in_stream  = in_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(out_ctx, nullptr);
        if (!out_stream) {
            std::fprintf(stderr, "Failed to allocate output stream\n");
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return 1;
        }
        if (avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
            std::fprintf(stderr, "Failed to copy codec parameters\n");
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return 1;
        }
        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = in_stream->time_base;

        // calcula passo nominal para timestamps sintéticos (no time_base do INPUT)
        AVRational fps = in_stream->avg_frame_rate.num ? in_stream->avg_frame_rate
                                                       : in_stream->r_frame_rate;
        step_ts[i] = fps_to_tb_step(fps, in_stream->time_base);
        if (step_ts[i] <= 0) {
            // fallback ultra defensivo: 1 unidade de time_base
            step_ts[i] = 1;
        }
    }

    // (4) Abrir arquivo de saída
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
            std::fprintf(stderr, "Could not open output file: %s\n", output_file);
            avformat_close_input(&in_ctx);
            avformat_free_context(out_ctx);
            return 1;
        }
    }

    // (5) Header com movflags (fMP4)
    AVDictionary* out_opts = nullptr;
    if (std::string(out_ctx->oformat->name) == "mp4") {
        av_dict_set(&out_opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    }
    if (avformat_write_header(out_ctx, &out_opts) < 0) {
        std::fprintf(stderr, "Error writing header\n");
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_close_input(&in_ctx);
        avformat_free_context(out_ctx);
        av_dict_free(&out_opts);
        return 1;
    }
    av_dict_free(&out_opts);

    // (6) Loop: remux
    AVPacket pkt;
    while (av_read_frame(in_ctx, &pkt) >= 0) {
        AVStream* in_stream  = in_ctx->streams[pkt.stream_index];
        AVStream* out_stream = out_ctx->streams[pkt.stream_index];

        // Preencher PTS/DTS/DURATION se vierem faltando (no time_base do INPUT)
        bool both_missing = (pkt.pts == AV_NOPTS_VALUE && pkt.dts == AV_NOPTS_VALUE);
        if (both_missing) {
            // gera timestamps sintéticos monotônicos
            pkt.dts = next_ts[pkt.stream_index];
            pkt.pts = next_ts[pkt.stream_index];
            if (pkt.duration == 0)
                pkt.duration = step_ts[pkt.stream_index];
            next_ts[pkt.stream_index] += step_ts[pkt.stream_index];
        } else {
            // se faltar um, espelha do outro
            if (pkt.pts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE)
                pkt.pts = pkt.dts;
            else if (pkt.dts == AV_NOPTS_VALUE && pkt.pts != AV_NOPTS_VALUE)
                pkt.dts = pkt.pts;

            // guarde um next_ts razoável se ainda não temos (para futuros pacotes sem ts)
            int64_t base = (pkt.dts != AV_NOPTS_VALUE) ? pkt.dts : pkt.pts;
            if (base != AV_NOPTS_VALUE) {
                // mantém next_ts ao menos >= base + step
                if (next_ts[pkt.stream_index] < base + step_ts[pkt.stream_index]) {
                    next_ts[pkt.stream_index] = base + step_ts[pkt.stream_index];
                }
            }
            if (pkt.duration == 0)
                pkt.duration = step_ts[pkt.stream_index];
        }

        // Reescala tudo para o time_base do OUTPUT
        if (pkt.pts != AV_NOPTS_VALUE) {
            pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base,
                                       (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        }
        if (pkt.dts != AV_NOPTS_VALUE) {
            pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base,
                                       (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        }
        if (pkt.duration > 0) {
            pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        }
        pkt.pos = -1;

        if (av_interleaved_write_frame(out_ctx, &pkt) < 0) {
            av_packet_unref(&pkt);
            std::fprintf(stderr, "Error muxing packet (stream %d)\n", pkt.stream_index);
            break;
        }
        av_packet_unref(&pkt);
    }

    // (7) Finalizar
    av_write_trailer(out_ctx);
    avformat_close_input(&in_ctx);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
    avformat_free_context(out_ctx);
    return 0;
}
