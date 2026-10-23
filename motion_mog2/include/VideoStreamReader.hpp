#pragma once

#include "AvPacket.hpp"
#include "VideoStreamParameters.hpp"

#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FfmpegRawVideoSource;

struct DecodedFrame {
    cv::cuda::GpuMat decoded_frame_gpu;
    std::uint64_t decoded_frame_index = 0;
};

class VideoStreamReader {
public:
    explicit VideoStreamReader(const std::string& input_source);

    [[nodiscard]]
    bool read(
        DecodedFrame& decoded_frame,
        std::vector<AvPacketPtr>& encoded_packets,
        cv::cuda::Stream& cuda_stream
    );

    std::shared_ptr<const VideoStreamParameters> videoStreamParameters() const;

    double fps() const;
    int width() const;
    int height() const;
    std::uint64_t decodedFrameCount() const;

private:
    // Declarada antes do reader para permanecer viva durante sua destruicao.
    cv::Ptr<FfmpegRawVideoSource> ffmpeg_video_source_;
    cv::Ptr<cv::cudacodec::VideoReader> nvdec_video_reader_;
    std::size_t decoded_frame_retrieve_index_ = 0;

    double stream_fps_ = 0.0;
    int decoded_frame_width_ = 0;
    int decoded_frame_height_ = 0;
    std::uint64_t next_decoded_frame_index_ = 0;
};
