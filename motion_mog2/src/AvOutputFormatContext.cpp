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
