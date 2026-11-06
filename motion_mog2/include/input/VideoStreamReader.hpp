#pragma once

#include "ffmpeg/VideoStreamParameters.hpp"

#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class EncodedVideoBuffer;
class FfmpegEncodedVideoSource;

struct DecodedFrame {
    cv::cuda::GpuMat decoded_frame_gpu;
    std::uint64_t decoded_frame_index = 0;
};

class VideoStreamReader {
public:
    // Os pacotes codificados nao passam pelo reader: a fonte FFmpeg os guarda
    // diretamente em encoded_video_buffer, e o main os retira de la.
    VideoStreamReader(
        const std::string& input_source,
        std::shared_ptr<EncodedVideoBuffer> encoded_video_buffer
    );

    [[nodiscard]]
    bool read(DecodedFrame& decoded_frame, cv::cuda::Stream& cuda_stream);

    std::shared_ptr<const VideoStreamParameters> getVideoStreamParameters() const;

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
