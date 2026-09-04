#include "EncodedVideoBuffer.hpp"

#include <chrono>

void EncodedVideoBuffer::updateCurrentGop(
    const std::vector<EncodedPacket>& encoded_packets,
    uint64_t frame_index
) {
    for (const EncodedPacket& encoded_packet : encoded_packets) {
        if (encoded_packet.has_key_frame) {
            current_gop_buffer_.clear();
            current_gop_bytes_ = 0;
            current_gop_has_key_frame_ = true;
            current_gop_start_frame_ = frame_index;
        }

        if (current_gop_has_key_frame_) {
            current_gop_bytes_ += encoded_packet.data.size();
            current_gop_buffer_.push_back(encoded_packet);
        }
    }
}

MotionBufferStartInfo EncodedVideoBuffer::startMotion(
    uint64_t motion_frame,
    const std::vector<EncodedPacket>& current_encoded_packets
) {
    motion_packet_buffer_.clear();
    motion_buffer_bytes_ = 0;
    motion_extra_frames_before_start_ = 0;

    MotionBufferStartInfo info;
    info.motion_frame = motion_frame;

    if (current_gop_has_key_frame_ && !current_gop_buffer_.empty()) {
        motion_packet_buffer_ = current_gop_buffer_;
        motion_buffer_bytes_ = current_gop_bytes_;
        motion_buffer_start_frame_ = current_gop_start_frame_;
        motion_extra_frames_before_start_ =
            motion_frame - current_gop_start_frame_;
    } else {
        motion_buffer_start_frame_ = motion_frame;
        for (const EncodedPacket& encoded_packet : current_encoded_packets) {
            motion_buffer_bytes_ += encoded_packet.data.size();
            motion_packet_buffer_.push_back(encoded_packet);
        }
    }

    info.start_frame = motion_buffer_start_frame_;
    info.extra_frames_before_motion = motion_extra_frames_before_start_;
    info.gop_packets = motion_packet_buffer_.size();
    info.starts_with_key_frame =
        !motion_packet_buffer_.empty() &&
        motion_packet_buffer_.front().has_key_frame;

    return info;
}

void EncodedVideoBuffer::appendMotionPackets(
    const std::vector<EncodedPacket>& encoded_packets
) {
    for (const EncodedPacket& encoded_packet : encoded_packets) {
        motion_buffer_bytes_ += encoded_packet.data.size();
        motion_packet_buffer_.push_back(encoded_packet);
    }
}

MotionBufferCompleteInfo EncodedVideoBuffer::buildCurrentMotionInfo() const {
    MotionBufferCompleteInfo info;
    info.packets = motion_packet_buffer_.size();
    info.bytes = motion_buffer_bytes_;
    info.extra_frames_before_motion = motion_extra_frames_before_start_;

    if (motion_packet_buffer_.size() >= 2) {
        info.duration_seconds =
            std::chrono::duration<double>(
                motion_packet_buffer_.back().received_at -
                motion_packet_buffer_.front().received_at
            ).count();
    }

    for (const EncodedPacket& encoded_packet : motion_packet_buffer_) {
        if (encoded_packet.has_key_frame) {
            ++info.key_frames;
        }
    }

    info.starts_with_key_frame =
        !motion_packet_buffer_.empty() &&
        motion_packet_buffer_.front().has_key_frame;

    return info;
}

MotionBufferCompleteInfo EncodedVideoBuffer::finishMotion() {
    MotionBufferCompleteInfo info = buildCurrentMotionInfo();

    motion_packet_buffer_.clear();
    motion_buffer_bytes_ = 0;
    motion_extra_frames_before_start_ = 0;

    return info;
}

MotionBufferCompleteInfo EncodedVideoBuffer::currentMotionInfo() const {
    return buildCurrentMotionInfo();
}
