#pragma once

#include "EncodedPacket.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

struct MotionBufferStartInfo {
    uint64_t motion_decoded_frame_index = 0;
    uint64_t start_decoded_frame_index = 0;
    uint64_t extra_decoded_frames_before_motion = 0;
    size_t gop_encoded_packet_count = 0;
    bool starts_with_key_frame = false;
    uint64_t requested_pre_event_frame_count = 0;
    bool pre_event_history_sufficient = false;
};

struct MotionBufferCompleteInfo {
    size_t encoded_packet_count = 0;
    size_t encoded_byte_count = 0;
    double duration_seconds = 0.0;
    size_t key_frame_count = 0;
    bool starts_with_key_frame = false;
    uint64_t extra_decoded_frames_before_motion = 0;
};

class EncodedVideoBuffer {
public:
    explicit EncodedVideoBuffer(uint64_t pre_event_frame_count = 0);

    void updatePreEventBuffer(
        const std::vector<EncodedPacket>& encoded_packets,
        uint64_t decoded_frame_index
    );

    MotionBufferStartInfo startMotion(
        uint64_t motion_decoded_frame_index,
        const std::vector<EncodedPacket>& current_encoded_packets
    );

    void appendMotionPackets(const std::vector<EncodedPacket>& encoded_packets);

    MotionBufferCompleteInfo finishMotion();
    MotionBufferCompleteInfo currentMotionInfo() const;

private:
    struct EncodedGop {
        std::vector<EncodedPacket> encoded_packets;
        size_t encoded_byte_count = 0;
        uint64_t start_observed_decoded_frame_index = 0;
    };

    void startNewGop(uint64_t decoded_frame_index);
    void appendEncodedPacketToCurrentGop(const EncodedPacket& encoded_packet);
    void preserveCurrentGop();
    void discardExpiredGops(uint64_t decoded_frame_index);
    uint64_t desiredPreEventStart(uint64_t decoded_frame_index) const;

    void resetMotionBuffer();
    bool hasCurrentGopWithKeyFrame() const;
    void copyPreEventToMotionBuffer(uint64_t motion_decoded_frame_index);
    void startMotionBufferFromCurrentPackets(
        uint64_t motion_decoded_frame_index,
        const std::vector<EncodedPacket>& current_encoded_packets
    );
    MotionBufferStartInfo buildMotionBufferStartInfo(
        uint64_t motion_decoded_frame_index
    ) const;
    MotionBufferCompleteInfo buildCurrentMotionInfo() const;

    uint64_t pre_event_frame_count_ = 0;
    std::deque<EncodedGop> previous_encoded_gops_;

    std::vector<EncodedPacket> current_gop_encoded_packets_;
    size_t current_gop_encoded_byte_count_ = 0;
    bool current_gop_has_key_frame_ = false;
    uint64_t current_gop_start_decoded_frame_index_ = 0;

    std::vector<EncodedPacket> motion_encoded_packets_;
    size_t motion_encoded_byte_count_ = 0;
    uint64_t motion_start_decoded_frame_index_ = 0;
    uint64_t motion_extra_decoded_frames_before_start_ = 0;
};
