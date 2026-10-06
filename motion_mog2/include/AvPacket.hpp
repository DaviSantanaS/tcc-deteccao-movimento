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
[[nodiscard]] AvPacketPtr cloneAvPacket(const AVPacket& source);
