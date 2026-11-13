#pragma once

#include "buffers/EncodedPacketQueue.hpp"
#include "ffmpeg/VideoStreamParameters.hpp"
#include "input/PacketReadEnd.hpp"

#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class FfmpegEncodedVideoSource;

struct DecodedFrame {
    cv::cuda::GpuMat decoded_frame_gpu;
    std::uint64_t decoded_frame_index = 0;
};

class VideoStreamReader {
public:
    explicit VideoStreamReader(
        const std::string& input_source,
        const std::string& rtsp_transport = "tcp",
        const std::atomic<bool>* keep_running = nullptr
    );

    [[nodiscard]]
    bool read(
        DecodedFrame& decoded_frame,
        cv::cuda::Stream& cuda_stream
    );

    void stopDecoding();

    // Confiavel depois de stopDecoding(), que espera a thread de leitura terminar.
    PacketReadEnd getPacketReadEnd() const;

    std::shared_ptr<const VideoStreamParameters> getVideoStreamParameters() const;
    EncodedPacketQueue& getEncodedPacketQueue();

    double getFramesPerSecond() const;
    int getWidth() const;
    int getHeight() const;
    std::uint64_t getDecodedFrameCount() const;

private:
    cv::Ptr<FfmpegEncodedVideoSource> ffmpeg_video_source_;
    cv::Ptr<cv::cudacodec::VideoReader> nvdec_video_reader_;
    std::size_t decoded_frame_retrieval_index_ = 0;

    double stream_fps_ = 0.0;
    int decoded_frame_width_ = 0;
    int decoded_frame_height_ = 0;
    std::uint64_t next_decoded_frame_index_ = 0;
};
