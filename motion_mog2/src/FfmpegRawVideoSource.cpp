#include "FfmpegRawVideoSource.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

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
