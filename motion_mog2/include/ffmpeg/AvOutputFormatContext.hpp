#pragma once

#include "ffmpeg/AvOutputIoContext.hpp"
#include "ffmpeg/AvPacket.hpp"
#include "ffmpeg/EncodedVideoPackets.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>
#include <vector>

struct VideoStreamParameters;

struct AvOutputFormatContextDeleter {
    void operator()(AVFormatContext* output_context) const noexcept;
};

using AvOutputFormatContextPtr =
    std::unique_ptr<AVFormatContext, AvOutputFormatContextDeleter>;

[[nodiscard]] AvOutputFormatContextPtr allocateMp4OutputContext();

[[nodiscard]] AVStream& createMp4VideoStream(
    AVFormatContext& mp4_output_context,
    const VideoStreamParameters& video_stream_parameters
);

void writeMp4Header(
    AVFormatContext& mp4_output_context,
    const AvOutputIoContextPtr& output_io_context
);

void writeMp4Packet(
    AVFormatContext& mp4_output_context,
    AVPacket& encoded_packet
);

void writeMp4Packets(
    AVFormatContext& mp4_output_context,
    std::vector<EncodedVideoPackets> encoded_packet_batches
);

void writeMp4Packets(
    AVFormatContext& mp4_output_context,
    std::vector<AvPacketPtr> encoded_packets,
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters
);

void writeMp4Trailer(AVFormatContext& mp4_output_context);
