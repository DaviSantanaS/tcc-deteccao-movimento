#include "AvOutputFormatContext.hpp"
#include "AvPacket.hpp"
#include "VideoStreamParameters.hpp"

#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

void AvOutputFormatContextDeleter::operator()(
    AVFormatContext* output_context
) const noexcept {
    if (output_context) {
        avformat_free_context(output_context);
    }
}

AvOutputFormatContextPtr allocateMp4OutputContext() {
    AVFormatContext* allocated_output_context = nullptr;
    const int allocation_result = avformat_alloc_output_context2(
        &allocated_output_context, nullptr, "mp4", nullptr
    );
    AvOutputFormatContextPtr output_context{allocated_output_context};

    if (allocation_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel alocar o contexto MP4: erro " +
            std::to_string(allocation_result)
        );
    }
    if (!output_context) {
        throw std::bad_alloc{};
    }
    return output_context;
}

AVStream& createMp4VideoStream(
    AVFormatContext& mp4_output_context,
    const VideoStreamParameters& video_stream_parameters
) {
    if (!mp4_output_context.oformat || !mp4_output_context.oformat->name ||
        std::strcmp(mp4_output_context.oformat->name, "mp4") != 0 ||
        mp4_output_context.iformat != nullptr || mp4_output_context.pb != nullptr ||
        mp4_output_context.nb_streams != 0) {
        throw std::invalid_argument("O contexto deve ser uma saida MP4 sem arquivo aberto nem streams.");
    }
    if (!video_stream_parameters.codec_parameters) {
        throw std::invalid_argument("Os parametros de codec devem estar presentes.");
    }

    const AVCodecParameters& source_codec_parameters =
        *video_stream_parameters.codec_parameters;
    if (source_codec_parameters.codec_type != AVMEDIA_TYPE_VIDEO ||
        source_codec_parameters.codec_id != AV_CODEC_ID_H264 ||
        source_codec_parameters.width <= 0 || source_codec_parameters.height <= 0) {
        throw std::invalid_argument("A stream deve ser video H.264 com resolucao positiva.");
    }
    if (video_stream_parameters.packet_timestamp_time_base.num <= 0 ||
        video_stream_parameters.packet_timestamp_time_base.den <= 0) {
        throw std::invalid_argument("A base de tempo deve ser positiva.");
    }
    if (source_codec_parameters.extradata_size < 0 ||
        (source_codec_parameters.extradata_size > 0 && !source_codec_parameters.extradata)) {
        throw std::invalid_argument("O tamanho e os bytes da extradata devem ser coerentes.");
    }

    AVStream* output_stream = avformat_new_stream(&mp4_output_context, nullptr);
    if (!output_stream) {
        throw std::bad_alloc{};
    }
    const int codec_parameters_copy_result = avcodec_parameters_copy(
        output_stream->codecpar, &source_codec_parameters
    );
    if (codec_parameters_copy_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel copiar os parametros para a stream MP4: erro " +
            std::to_string(codec_parameters_copy_result)
        );
    }

    output_stream->codecpar->codec_tag = 0;
    output_stream->time_base = video_stream_parameters.packet_timestamp_time_base;
    return *output_stream;
}

void writeMp4Header(
    AVFormatContext& mp4_output_context,
    const AvOutputIoContextPtr& output_io_context
) {
    if (!mp4_output_context.oformat || !mp4_output_context.oformat->name ||
        std::strcmp(mp4_output_context.oformat->name, "mp4") != 0 ||
        mp4_output_context.iformat != nullptr || mp4_output_context.pb != nullptr ||
        mp4_output_context.nb_streams != 1 || !mp4_output_context.streams ||
        !mp4_output_context.streams[0]) {
        throw std::invalid_argument("O contexto deve ser MP4 com uma stream e sem I/O associado.");
    }

    const AVStream& video_output_stream = *mp4_output_context.streams[0];
    const AVCodecParameters* output_codec_parameters = video_output_stream.codecpar;
    if (!output_codec_parameters || output_codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
        output_codec_parameters->codec_id != AV_CODEC_ID_H264 ||
        output_codec_parameters->width <= 0 || output_codec_parameters->height <= 0 ||
        video_output_stream.time_base.num <= 0 || video_output_stream.time_base.den <= 0 ||
        output_codec_parameters->extradata_size < 0 ||
        (output_codec_parameters->extradata_size > 0 && !output_codec_parameters->extradata)) {
        throw std::invalid_argument("A stream H.264 deve ter parametros, resolucao, base de tempo e extradata coerentes.");
    }
    if (!output_io_context || !output_io_context->write_flag ||
        !(output_io_context->seekable & AVIO_SEEKABLE_NORMAL) || output_io_context->error < 0) {
        throw std::invalid_argument("O I/O deve estar aberto para escrita, permitir seek e nao ter erro anterior.");
    }

    mp4_output_context.pb = output_io_context.get();
    // O muxer pode alterar time_base; os pacotes devem usar o valor resultante.
    const int header_write_result = avformat_write_header(&mp4_output_context, nullptr);
    if (header_write_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel escrever o cabecalho MP4: erro " + std::to_string(header_write_result)
        );
    }
    if (output_io_context->error < 0) {
        throw std::runtime_error(
            "O I/O falhou ao escrever o cabecalho MP4: erro " + std::to_string(output_io_context->error)
        );
    }
}

void writeMp4Packet(
    AVFormatContext& mp4_output_context,
    AVPacket& encoded_packet
) {
    if (!mp4_output_context.oformat || !mp4_output_context.oformat->name ||
        std::strcmp(mp4_output_context.oformat->name, "mp4") != 0 ||
        mp4_output_context.iformat != nullptr || mp4_output_context.nb_streams != 1 ||
        !mp4_output_context.streams || !mp4_output_context.streams[0] ||
        !mp4_output_context.pb || !mp4_output_context.pb->write_flag ||
        mp4_output_context.pb->error < 0) {
        throw std::invalid_argument("A saida deve ser MP4 com uma stream e I/O de escrita sem erro anterior.");
    }

    const AVStream& video_output_stream = *mp4_output_context.streams[0];
    if (!video_output_stream.codecpar ||
        video_output_stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        video_output_stream.codecpar->codec_id != AV_CODEC_ID_H264 ||
        video_output_stream.time_base.num <= 0 || video_output_stream.time_base.den <= 0) {
        throw std::invalid_argument("A stream de saida deve ser video H.264 com base de tempo positiva.");
    }
    if (!encoded_packet.buf || !encoded_packet.data || encoded_packet.size <= 0 ||
        encoded_packet.stream_index != video_output_stream.index ||
        encoded_packet.time_base.num <= 0 || encoded_packet.time_base.den <= 0 ||
        av_cmp_q(encoded_packet.time_base, video_output_stream.time_base) != 0 ||
        encoded_packet.pts == AV_NOPTS_VALUE || encoded_packet.dts == AV_NOPTS_VALUE ||
        encoded_packet.duration < 0) {
        throw std::invalid_argument("O pacote deve ter dados refcountados e timestamps preparados para a stream de saida.");
    }

    const int packet_write_result = av_interleaved_write_frame(
        &mp4_output_context, &encoded_packet
    );
    if (packet_write_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel escrever o pacote MP4: erro " + std::to_string(packet_write_result)
        );
    }
    if (mp4_output_context.pb->error < 0) {
        throw std::runtime_error(
            "O I/O falhou ao escrever o pacote MP4: erro " + std::to_string(mp4_output_context.pb->error)
        );
    }
}

void writeMp4Trailer(AVFormatContext& mp4_output_context) {
    if (!mp4_output_context.oformat || !mp4_output_context.oformat->name ||
        std::strcmp(mp4_output_context.oformat->name, "mp4") != 0 ||
        mp4_output_context.iformat != nullptr || mp4_output_context.nb_streams != 1 ||
        !mp4_output_context.streams || !mp4_output_context.streams[0] ||
        !mp4_output_context.pb || !mp4_output_context.pb->write_flag ||
        !(mp4_output_context.pb->seekable & AVIO_SEEKABLE_NORMAL) ||
        mp4_output_context.pb->error < 0) {
        throw std::invalid_argument("A saida deve ser MP4 com uma stream e I/O de escrita com seek e sem erro anterior.");
    }
    const AVCodecParameters* output_codec_parameters = mp4_output_context.streams[0]->codecpar;
    if (!output_codec_parameters || output_codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
        output_codec_parameters->codec_id != AV_CODEC_ID_H264) {
        throw std::invalid_argument("A stream de saida deve ser video H.264.");
    }

    AVIOContext* const output_io_context = mp4_output_context.pb;
    const int trailer_write_result = av_write_trailer(&mp4_output_context);
    mp4_output_context.pb = nullptr;
    if (trailer_write_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel finalizar o MP4: erro " + std::to_string(trailer_write_result)
        );
    }
    if (output_io_context->error < 0) {
        throw std::runtime_error(
            "O I/O falhou ao finalizar o MP4: erro " + std::to_string(output_io_context->error)
        );
    }
}
