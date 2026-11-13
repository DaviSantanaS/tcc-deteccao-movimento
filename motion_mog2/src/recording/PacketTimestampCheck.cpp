#include "recording/PacketTimestampCheck.hpp"

#include <cstdint>
#include <optional>

extern "C" {
#include <libavutil/avutil.h>
}

PacketTimestampIssue checkPacketTimestampsForMuxing(
    const AVPacket& encoded_packet,
    std::optional<std::int64_t> previous_written_dts
) {
    if (encoded_packet.pts == AV_NOPTS_VALUE || encoded_packet.dts == AV_NOPTS_VALUE) {
        return PacketTimestampIssue::missing_timestamp;
    }
    if (encoded_packet.pts < encoded_packet.dts) {
        return PacketTimestampIssue::pts_before_dts;
    }
    if (previous_written_dts && encoded_packet.dts <= *previous_written_dts) {
        return PacketTimestampIssue::non_increasing_dts;
    }
    return PacketTimestampIssue::none;
}
