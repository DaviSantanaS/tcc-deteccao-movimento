#pragma once

enum class PacketReadEndReason { not_ended, end_of_stream, interrupted, read_error, packet_preparation_error, decoder_stalled };

struct PacketReadEnd {
    PacketReadEndReason reason = PacketReadEndReason::not_ended;
    int ffmpeg_error_code = 0;
};
