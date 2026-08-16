#pragma once

#include "VideoStreamReader.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct MotionBufferStartInfo {
    uint64_t motion_frame = 0;
    uint64_t start_frame = 0;
    uint64_t extra_frames_before_motion = 0;
    size_t gop_packets = 0;
    bool starts_with_key_frame = false;
};

struct MotionBufferCompleteInfo {
    size_t packets = 0;
    size_t bytes = 0;
    double duration_seconds = 0.0;
    size_t key_frames = 0;
    bool starts_with_key_frame = false;
    uint64_t extra_frames_before_motion = 0;
};

class EncodedVideoBuffer {
public:
    void updateCurrentGop(
        const std::vector<EncodedPacket>& encoded_packets,
        uint64_t frame_index
    );

    MotionBufferStartInfo startMotion(
        uint64_t motion_frame,
        const std::vector<EncodedPacket>& current_encoded_packets
    );

    void appendMotionPackets(const std::vector<EncodedPacket>& encoded_packets);

    MotionBufferCompleteInfo finishMotion();
    MotionBufferCompleteInfo currentMotionInfo() const;

private:
    MotionBufferCompleteInfo buildCurrentMotionInfo() const;

    std::vector<EncodedPacket> current_gop_buffer_;
    size_t current_gop_bytes_ = 0;
    bool current_gop_has_key_frame_ = false;
    uint64_t current_gop_start_frame_ = 0;

    std::vector<EncodedPacket> motion_packet_buffer_;
    size_t motion_buffer_bytes_ = 0;
    uint64_t motion_buffer_start_frame_ = 0;
    uint64_t motion_extra_frames_before_start_ = 0;
};
