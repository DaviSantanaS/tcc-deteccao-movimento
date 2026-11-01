#pragma once

extern "C" {
#include <libavcodec/packet.h>
}

#include <memory>

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const noexcept;
};

using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

[[nodiscard]] AvPacketPtr allocateAvPacket();
[[nodiscard]] AvPacketPtr cloneAvPacket(const AVPacket& source_packet);

void prepareEncodedPacketForOutput(
    AVPacket& encoded_packet,
    AVRational source_time_base,
    AVRational output_time_base,
    int output_stream_index
);
