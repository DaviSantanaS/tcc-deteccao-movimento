#include "EncodedVideoBuffer.hpp"

#include <chrono>
#include <utility>

EncodedVideoBuffer::EncodedVideoBuffer(uint64_t pre_event_frame_count) :
    pre_event_frame_count_(pre_event_frame_count) {
}

void EncodedVideoBuffer::updatePreEventBuffer(
    const std::vector<EncodedPacket>& encoded_packets,
    uint64_t decoded_frame_index
) {
    for (const EncodedPacket& encoded_packet : encoded_packets) {
        if (encoded_packet.has_key_frame) {
            startNewGop(decoded_frame_index);
        }

        appendEncodedPacketToCurrentGop(encoded_packet);
    }

    discardExpiredGops(decoded_frame_index);
}

void EncodedVideoBuffer::startNewGop(uint64_t decoded_frame_index) {
    preserveCurrentGop();
    current_gop_encoded_packets_.clear();
    current_gop_encoded_byte_count_ = 0;
    current_gop_has_key_frame_ = true;
    current_gop_start_decoded_frame_index_ = decoded_frame_index;
}

void EncodedVideoBuffer::preserveCurrentGop() {
    if (pre_event_frame_count_ == 0 || !hasCurrentGopWithKeyFrame()) {
        return;
    }

    EncodedGop previous_gop;
    previous_gop.encoded_packets = std::move(current_gop_encoded_packets_);
    previous_gop.encoded_byte_count = current_gop_encoded_byte_count_;
    previous_gop.start_observed_decoded_frame_index =
        current_gop_start_decoded_frame_index_;
    previous_encoded_gops_.push_back(std::move(previous_gop));
}

uint64_t EncodedVideoBuffer::desiredPreEventStart(uint64_t decoded_frame_index) const {
    return decoded_frame_index >= pre_event_frame_count_
        ? decoded_frame_index - pre_event_frame_count_
        : 0;
}

void EncodedVideoBuffer::discardExpiredGops(uint64_t decoded_frame_index) {
    const uint64_t desired_start = desiredPreEventStart(decoded_frame_index);

    if (pre_event_frame_count_ == 0 ||
        (hasCurrentGopWithKeyFrame() &&
         current_gop_start_decoded_frame_index_ <= desired_start)) {
        previous_encoded_gops_.clear();
        return;
    }

    while (previous_encoded_gops_.size() > 1 &&
           previous_encoded_gops_[1].start_observed_decoded_frame_index <= desired_start) {
        previous_encoded_gops_.pop_front();
    }
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
    discardExpiredGops(motion_decoded_frame_index);

    if (hasCurrentGopWithKeyFrame()) {
        copyPreEventToMotionBuffer(motion_decoded_frame_index);
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

void EncodedVideoBuffer::copyPreEventToMotionBuffer(
    uint64_t motion_decoded_frame_index
) {
    motion_start_decoded_frame_index_ = previous_encoded_gops_.empty()
        ? current_gop_start_decoded_frame_index_
        : previous_encoded_gops_.front().start_observed_decoded_frame_index;

    for (const EncodedGop& previous_gop : previous_encoded_gops_) {
        motion_encoded_packets_.insert(
            motion_encoded_packets_.end(),
            previous_gop.encoded_packets.begin(),
            previous_gop.encoded_packets.end()
        );
        motion_encoded_byte_count_ += previous_gop.encoded_byte_count;
    }

    appendMotionPackets(current_gop_encoded_packets_);
    motion_extra_decoded_frames_before_start_ =
        motion_decoded_frame_index - motion_start_decoded_frame_index_;
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
    info.requested_pre_event_frame_count = pre_event_frame_count_;
    info.pre_event_history_sufficient =
        info.starts_with_key_frame &&
        motion_extra_decoded_frames_before_start_ >= pre_event_frame_count_;

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
