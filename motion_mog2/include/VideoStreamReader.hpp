#pragma once

#include "EncodedPacket.hpp"

#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DecodedFrame {
    cv::cuda::GpuMat decoded_frame_gpu;
    uint64_t decoded_frame_index = 0;
};

struct EncodedPacketBatch {
    std::vector<EncodedPacket> encoded_packets;
};

class VideoStreamReader {
public:
    explicit VideoStreamReader(const std::string& rtsp_url);

    bool read(
        DecodedFrame& decoded_frame,
        EncodedPacketBatch& encoded_batch,
        cv::cuda::Stream& cuda_stream
    );

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
