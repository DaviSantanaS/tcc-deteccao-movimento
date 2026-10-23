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
    uint64_t decoded_frame_index = 0;
};

class VideoStreamReader {
public:
    explicit VideoStreamReader(const std::string& rtsp_url);

    bool read(
        DecodedFrame& decoded_frame,
        std::vector<AvPacketPtr>& encoded_packets,
        cv::cuda::Stream& cuda_stream
    );

    std::shared_ptr<const VideoStreamParameters> videoStreamParameters() const;

    double fps() const;
    int width() const;
    int height() const;
    uint64_t processedFrameCount() const;

private:
    // Declarada antes do reader para permanecer viva durante sua destruicao.
    cv::Ptr<FfmpegRawVideoSource> ffmpeg_video_source_;
    cv::Ptr<cv::cudacodec::VideoReader> nvdec_video_reader_;
    size_t decoded_frame_retrieve_index_ = 0;

    double stream_fps_ = 0.0;
    int decoded_frame_width_ = 0;
    int decoded_frame_height_ = 0;
    uint64_t next_decoded_frame_index_ = 0;
};
