#include "EncodedVideoBuffer.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <limits>

namespace {

size_t packetByteSize(const AVPacket& packet) {
    return packet.size > 0 ? static_cast<size_t>(packet.size) : 0;
}

bool isKeyFrame(const AVPacket& packet) {
    return (packet.flags & AV_PKT_FLAG_KEY) != 0;
}

double packetSequenceDurationSeconds(
    const std::vector<AvPacketPtr>& packets
) {
    int64_t first_timestamp_us = std::numeric_limits<int64_t>::max();
    int64_t last_timestamp_us = std::numeric_limits<int64_t>::min();

    for (const AvPacketPtr& packet : packets) {
        if (!packet || packet->time_base.num <= 0 || packet->time_base.den <= 0) {
            continue;
        }

        const int64_t timestamp =
            packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (timestamp == AV_NOPTS_VALUE) {
            continue;
        }

        const int64_t duration = std::max<int64_t>(packet->duration, 0);
        const int64_t start_us = av_rescale_q(
            timestamp,
            packet->time_base,
            AV_TIME_BASE_Q
        );
        const int64_t end_us = av_rescale_q(
            timestamp + duration,
            packet->time_base,
            AV_TIME_BASE_Q
        );

        first_timestamp_us = std::min(first_timestamp_us, start_us);
        last_timestamp_us = std::max(last_timestamp_us, end_us);
    }

    if (first_timestamp_us == std::numeric_limits<int64_t>::max() ||
        last_timestamp_us <= first_timestamp_us) {
        return 0.0;
    }

    return static_cast<double>(last_timestamp_us - first_timestamp_us) /
           static_cast<double>(AV_TIME_BASE);
}

}  // namespace

void EncodedVideoBuffer::updateCurrentGop(
    const std::vector<AvPacketPtr>& encoded_packets,
    uint64_t decoded_frame_index
) {
    for (const AvPacketPtr& encoded_packet : encoded_packets) {
        if (!encoded_packet) {
            continue;
        }

        if (isKeyFrame(*encoded_packet)) {
            startNewGop(decoded_frame_index);
        }

        appendEncodedPacketToCurrentGop(*encoded_packet);
    }
}

void EncodedVideoBuffer::startNewGop(uint64_t decoded_frame_index) {
    current_gop_encoded_packets_.clear();
    current_gop_encoded_byte_count_ = 0;
    current_gop_has_key_frame_ = true;
    current_gop_start_decoded_frame_index_ = decoded_frame_index;
}

void EncodedVideoBuffer::appendEncodedPacketToCurrentGop(
    const AVPacket& encoded_packet
) {
    current_gop_encoded_byte_count_ += packetByteSize(encoded_packet);
    current_gop_encoded_packets_.push_back(cloneAvPacket(encoded_packet));
}

MotionBufferStartInfo EncodedVideoBuffer::startMotion(
    uint64_t motion_decoded_frame_index,
    const std::vector<AvPacketPtr>& current_encoded_packets
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
    motion_encoded_packets_.clear();
    motion_encoded_packets_.reserve(current_gop_encoded_packets_.size());
    for (const AvPacketPtr& encoded_packet : current_gop_encoded_packets_) {
        if (encoded_packet) {
            motion_encoded_packets_.push_back(
                cloneAvPacket(*encoded_packet)
            );
        }
    }

    motion_encoded_byte_count_ = current_gop_encoded_byte_count_;
    motion_start_decoded_frame_index_ = current_gop_start_decoded_frame_index_;
    motion_extra_decoded_frames_before_start_ =
        motion_decoded_frame_index - current_gop_start_decoded_frame_index_;
}

void EncodedVideoBuffer::startMotionBufferFromCurrentPackets(
    uint64_t motion_decoded_frame_index,
    const std::vector<AvPacketPtr>& current_encoded_packets
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
        isKeyFrame(*motion_encoded_packets_.front());

    return info;
}

void EncodedVideoBuffer::appendMotionPackets(
    const std::vector<AvPacketPtr>& encoded_packets
) {
    for (const AvPacketPtr& encoded_packet : encoded_packets) {
        if (!encoded_packet) {
            continue;
        }

        motion_encoded_byte_count_ += packetByteSize(*encoded_packet);
        motion_encoded_packets_.push_back(cloneAvPacket(*encoded_packet));
    }
}

MotionBufferCompleteInfo EncodedVideoBuffer::buildCurrentMotionInfo() const {
    MotionBufferCompleteInfo info;
    info.encoded_packet_count = motion_encoded_packets_.size();
    info.encoded_byte_count = motion_encoded_byte_count_;
    info.extra_decoded_frames_before_motion =
        motion_extra_decoded_frames_before_start_;

    info.duration_seconds =
        packetSequenceDurationSeconds(motion_encoded_packets_);

    for (const AvPacketPtr& encoded_packet : motion_encoded_packets_) {
        if (encoded_packet && isKeyFrame(*encoded_packet)) {
            ++info.key_frame_count;
        }
    }

    info.starts_with_key_frame =
        !motion_encoded_packets_.empty() &&
        isKeyFrame(*motion_encoded_packets_.front());

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
