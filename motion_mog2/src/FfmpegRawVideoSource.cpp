#include "FfmpegRawVideoSource.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
}

#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

std::string ffmpegErrorMessage(int error_code) {
    char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

}  // namespace

void FfmpegRawVideoSource::AvFormatContextDeleter::operator()(
    AVFormatContext* input_context
) const noexcept {
    avformat_close_input(&input_context);
}

FfmpegRawVideoSource::FfmpegRawVideoSource(const std::string& input_url)
    : current_encoded_frame_packet(allocateAvPacket()) {
    openInput(input_url);
    initializeOpenCvFormat();
    copyCodecExtraDataFromStream();
}

void FfmpegRawVideoSource::openInput(const std::string& input_url) {
    AVFormatContext* opened_input_format_context = nullptr;
    const int open_input_result = avformat_open_input(
        &opened_input_format_context,
        input_url.c_str(),
        nullptr,
        nullptr
    );
    AvFormatContextPtr input_format_context{opened_input_format_context};

    if (open_input_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel abrir a entrada com FFmpeg: " +
            ffmpegErrorMessage(open_input_result)
        );
    }

    const int find_stream_info_result =
        avformat_find_stream_info(input_format_context.get(), nullptr);
    if (find_stream_info_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel obter informacoes dos streams: " +
            ffmpegErrorMessage(find_stream_info_result)
        );
    }

    const int selected_video_stream_index = av_find_best_stream(
        input_format_context.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0
    );
    if (selected_video_stream_index < 0) {
        throw std::runtime_error(
            "Nenhum stream de video foi encontrado: " +
            ffmpegErrorMessage(selected_video_stream_index)
        );
    }

    encoded_video_stream =
        input_format_context->streams[selected_video_stream_index];
    encoded_video_stream_index = selected_video_stream_index;
    video_source_context = std::move(input_format_context);
}

void FfmpegRawVideoSource::initializeOpenCvFormat() {
    if (encoded_video_stream == nullptr ||
        encoded_video_stream->codecpar == nullptr) {
        throw std::runtime_error(
            "O stream de video nao possui parametros de codec."
        );
    }

    const AVCodecID codec_id = encoded_video_stream->codecpar->codec_id;
    if (codec_id != AV_CODEC_ID_H264) {
        throw std::runtime_error(
            "Codec de video nao suportado: " +
            std::string(avcodec_get_name(codec_id))
        );
    }

    video_format.codec = cv::cudacodec::Codec::H264;
    const AVCodecParameters* codec_parameters =
    encoded_video_stream->codecpar;

    if (codec_parameters->width <= 0 || codec_parameters->height <= 0) {
        throw std::runtime_error("Resolucao invalida informada pelo stream.");
    }

    const AVRational frame_rate = av_guess_frame_rate(
        video_source_context.get(),
        encoded_video_stream,
        nullptr
    );

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        throw std::runtime_error("Nao foi possivel determinar o FPS do stream.");
    }

    video_format.width = codec_parameters->width;
    video_format.height = codec_parameters->height;
    video_format.displayArea = cv::Rect(
        0, 0, video_format.width, video_format.height
    );
    video_format.fps = av_q2d(frame_rate);
}

cv::cudacodec::FormatInfo FfmpegRawVideoSource::format() const {
    return video_format;
}

void FfmpegRawVideoSource::copyCodecExtraDataFromStream() {
    const AVCodecParameters* codec_parameters =
        encoded_video_stream->codecpar;

    if (codec_parameters->extradata == nullptr ||
        codec_parameters->extradata_size <= 0) {
        video_codec_extra_data.release();
        return;
    }

    video_codec_extra_data.create(
        1,
        codec_parameters->extradata_size,
        CV_8UC1
    );
    std::memcpy(
        video_codec_extra_data.data,
        codec_parameters->extradata,
        static_cast<size_t>(codec_parameters->extradata_size)
    );
}

void FfmpegRawVideoSource::getExtraData(cv::Mat& extra_data) const {
    video_codec_extra_data.copyTo(extra_data);
}
