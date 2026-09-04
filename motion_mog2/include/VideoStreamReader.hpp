#pragma once

#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct EncodedPacket {
    std::vector<unsigned char> data;
    std::chrono::steady_clock::time_point received_at;
    bool has_key_frame = false;
};

struct VideoFrameData {
    cv::cuda::GpuMat decoded_frame_gpu;
    std::vector<EncodedPacket> encoded_packets;
    uint64_t decoded_frame_index = 0;
};

class VideoStreamReader {
public:
    explicit VideoStreamReader(const std::string& rtsp_url);

    bool read(VideoFrameData& frame_data, cv::cuda::Stream& cuda_stream);

    double fps() const;
    int width() const;
    int height() const;
    uint64_t processedFrameCount() const;

private:
    cv::Ptr<cv::cudacodec::VideoReader> video_reader_;
    size_t decoded_frame_retrieve_index_ = 0;
    size_t encoded_packet_base_index_ = 0;

    double stream_fps_ = 0.0;
    int decoded_frame_width_ = 0;
    int decoded_frame_height_ = 0;
    uint64_t next_decoded_frame_index_ = 0;
};
