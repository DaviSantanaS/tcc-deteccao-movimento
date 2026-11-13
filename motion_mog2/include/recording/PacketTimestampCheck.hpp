#pragma once

extern "C" {
#include <libavcodec/packet.h>
}

#include <cstdint>
#include <optional>

enum class PacketTimestampIssue { none, missing_timestamp, pts_before_dts, non_increasing_dts };

PacketTimestampIssue checkPacketTimestampsForMuxing(
    const AVPacket& encoded_packet,
    std::optional<std::int64_t> previous_written_dts
);
