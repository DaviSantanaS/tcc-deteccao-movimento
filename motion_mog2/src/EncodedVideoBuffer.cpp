#include "EncodedVideoBuffer.hpp"

#include <chrono>

void EncodedVideoBuffer::updateCurrentGop(
    const std::vector<EncodedPacket>& encoded_packets,
    uint64_t decoded_frame_index
) {
    for (const EncodedPacket& encoded_packet : encoded_packets) {
        if (encoded_packet.has_key_frame) {
            startNewGop(decoded_frame_index);
        }

        appendEncodedPacketToCurrentGop(encoded_packet);
    }
}

void EncodedVideoBuffer::startNewGop(uint64_t decoded_frame_index) {
    current_gop_encoded_packets_.clear();
    current_gop_encoded_byte_count_ = 0;
    current_gop_has_key_frame_ = true;
    current_gop_start_decoded_frame_index_ = decoded_frame_index;
}

void EncodedVideoBuffer::appendEncodedPacketToCurrentGop(
    const EncodedPacket& encoded_packet
) {
    current_gop_encoded_byte_count_ += encoded_packet.data.size();
    current_gop_encoded_packets_.push_back(encoded_packet);
}

MotionBufferStartInfo EncodedVideoBuffer::startMotion(
    uint64_t motion_decoded_frame_index,
    const std::vector<EncodedPacket>& current_encoded_packets
) {
    resetMotionBuffer();

    if (hasCurrentGopWithKeyFrame()) {
        copyCurrentGopToMotionBuffer(motion_decoded_frame_index);
    } else {
        startMotionBufferFromCurrentPackets(
            motion_decoded_frame_index,
            current_encoded_packets
        );
    }

    return buildMotionBufferStartInfo(motion_decoded_frame_index);
}

void EncodedVideoBuffer::resetMotionBuffer() {
    motion_encoded_packets_.clear();
    motion_encoded_byte_count_ = 0;
    motion_extra_decoded_frames_before_start_ = 0;
}

bool EncodedVideoBuffer::hasCurrentGopWithKeyFrame() const {
    return current_gop_has_key_frame_ && !current_gop_encoded_packets_.empty();
}

void EncodedVideoBuffer::copyCurrentGopToMotionBuffer(
    uint64_t motion_decoded_frame_index
) {
    motion_encoded_packets_ = current_gop_encoded_packets_;
    motion_encoded_byte_count_ = current_gop_encoded_byte_count_;
    motion_start_decoded_frame_index_ = current_gop_start_decoded_frame_index_;
    motion_extra_decoded_frames_before_start_ =
        motion_decoded_frame_index - current_gop_start_decoded_frame_index_;
}

void EncodedVideoBuffer::startMotionBufferFromCurrentPackets(
    uint64_t motion_decoded_frame_index,
    const std::vector<EncodedPacket>& current_encoded_packets
) {
    motion_start_decoded_frame_index_ = motion_decoded_frame_index;
    appendMotionPackets(current_encoded_packets);
}

MotionBufferStartInfo EncodedVideoBuffer::buildMotionBufferStartInfo(
    uint64_t motion_decoded_frame_index
) const {
    MotionBufferStartInfo info;
    info.motion_decoded_frame_index = motion_decoded_frame_index;
    info.start_decoded_frame_index = motion_start_decoded_frame_index_;
    info.extra_decoded_frames_before_motion =
        motion_extra_decoded_frames_before_start_;
    info.gop_encoded_packet_count = motion_encoded_packets_.size();
    info.starts_with_key_frame =
        !motion_encoded_packets_.empty() &&
        motion_encoded_packets_.front().has_key_frame;

    return info;
}

void EncodedVideoBuffer::appendMotionPackets(
    const std::vector<EncodedPacket>& encoded_packets
) {
    for (const EncodedPacket& encoded_packet : encoded_packets) {
        motion_encoded_byte_count_ += encoded_packet.data.size();
        motion_encoded_packets_.push_back(encoded_packet);
    }
}

MotionBufferCompleteInfo EncodedVideoBuffer::buildCurrentMotionInfo() const {
    MotionBufferCompleteInfo info;
    info.encoded_packet_count = motion_encoded_packets_.size();
    info.encoded_byte_count = motion_encoded_byte_count_;
    info.extra_decoded_frames_before_motion =
        motion_extra_decoded_frames_before_start_;

    if (motion_encoded_packets_.size() >= 2) {
        info.duration_seconds =
            std::chrono::duration<double>(
                motion_encoded_packets_.back().received_at -
                motion_encoded_packets_.front().received_at
            ).count();
    }

    for (const EncodedPacket& encoded_packet : motion_encoded_packets_) {
        if (encoded_packet.has_key_frame) {
            ++info.key_frame_count;
        }
    }

    info.starts_with_key_frame =
        !motion_encoded_packets_.empty() &&
        motion_encoded_packets_.front().has_key_frame;

    return info;
}

MotionBufferCompleteInfo EncodedVideoBuffer::finishMotion() {
    MotionBufferCompleteInfo info = buildCurrentMotionInfo();

    motion_encoded_packets_.clear();
    motion_encoded_byte_count_ = 0;
    motion_extra_decoded_frames_before_start_ = 0;

    return info;
}

MotionBufferCompleteInfo EncodedVideoBuffer::currentMotionInfo() const {
    return buildCurrentMotionInfo();
}
