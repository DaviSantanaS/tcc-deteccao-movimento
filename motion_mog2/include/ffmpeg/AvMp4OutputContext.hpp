#pragma once

#include "ffmpeg/AvOutputFileContext.hpp"
#include "ffmpeg/AvPacket.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>

struct VideoStreamParameters;

struct AvMp4OutputContextDeleter {
    void operator()(AVFormatContext* output_context) const noexcept;
};

using AvMp4OutputContextPtr =
    std::unique_ptr<AVFormatContext, AvMp4OutputContextDeleter>;

[[nodiscard]] AvMp4OutputContextPtr allocateMp4OutputContext();

[[nodiscard]] AVStream& createMp4VideoStream(
    AVFormatContext& mp4_output_context,
    const VideoStreamParameters& video_stream_parameters
);

void writeMp4Header(
    AVFormatContext& mp4_output_context,
    const AvOutputFileContextPtr& output_io_context
);

void writeMp4Packet(
    AVFormatContext& mp4_output_context,
    AVPacket& encoded_packet
);

void writeMp4Trailer(AVFormatContext& mp4_output_context);
