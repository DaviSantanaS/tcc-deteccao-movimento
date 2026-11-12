#pragma once

#include "buffers/EncodedPacketQueue.hpp"
#include "detectors/MotionHysteresis.hpp"
#include "recording/EventPacketSink.hpp"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

enum class RecordingState { idle, waiting_keyframe, recording, post_event, closing_at_keyframe };

struct EventRecorderConfig {
    std::size_t lookback_packet_count = 0;
    std::size_t maximum_idle_packet_count = 0;
    std::size_t post_event_packet_count = 0;
};

class EventRecorder {
public:
    EventRecorder(
        EncodedPacketQueue& encoded_packet_queue,
        EventPacketSink& event_packet_sink,
        EventRecorderConfig recorder_config,
        std::ostream& event_log
    );

    void processDecodedFrame(const MotionState& motion_state, std::uint64_t decoded_frame_index);
    void finishCapture(std::vector<AvPacketPtr> remaining_encoded_packets);
    RecordingState getRecordingState() const;
    std::size_t getCompletedRecordingCount() const;

private:
    void openRecording();
    void writePackets(std::vector<AvPacketPtr>&& encoded_packets);
    void finalizeRecording();

    EncodedPacketQueue& encoded_packet_queue_;
    EventPacketSink& event_packet_sink_;
    EventRecorderConfig recorder_config_;
    std::ostream& event_log_;
    std::size_t recording_read_count_ = 0;
    std::size_t recording_encoded_packet_count_ = 0;
    std::size_t completed_recording_count_ = 0;
    RecordingState recording_state_ = RecordingState::idle;
    std::size_t post_remaining_packet_count_ = 0;
};
