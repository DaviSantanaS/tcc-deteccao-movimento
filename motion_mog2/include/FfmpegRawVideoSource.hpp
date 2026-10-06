#pragma once

#include "AvPacket.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <opencv2/cudacodec.hpp>

#include <memory>
#include <string>

class FfmpegRawVideoSource {
public:
    explicit FfmpegRawVideoSource(const std::string& input_url);

    cv::cudacodec::FormatInfo format() const;
    void getExtraData(cv::Mat& extra_data) const;

    FfmpegRawVideoSource(const FfmpegRawVideoSource&) = delete;
    FfmpegRawVideoSource& operator=(const FfmpegRawVideoSource&) = delete;
    FfmpegRawVideoSource(FfmpegRawVideoSource&&) = delete;
    FfmpegRawVideoSource& operator=(FfmpegRawVideoSource&&) = delete;

private:
    struct AvFormatContextDeleter {
        void operator()(AVFormatContext* input_context) const noexcept;
    };

    using AvFormatContextPtr =
        std::unique_ptr<AVFormatContext, AvFormatContextDeleter>;

    void openInput(const std::string& input_url);
    void initializeOpenCvFormat();
    void copyCodecExtraDataFromStream();

    AvFormatContextPtr video_source_context;
    AvPacketPtr current_encoded_frame_packet;
    AVStream* encoded_video_stream = nullptr;
    int encoded_video_stream_index = -1;
    cv::cudacodec::FormatInfo video_format;
    cv::Mat video_codec_extra_data;

};
