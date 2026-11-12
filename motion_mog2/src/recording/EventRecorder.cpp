#include "recording/EventRecorder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>

EventRecorder::EventRecorder(
    EncodedPacketQueue& encoded_packet_queue,
    EventPacketSink& event_packet_sink,
    EventRecorderConfig recorder_config,
    std::ostream& event_log
) :
    encoded_packet_queue_(encoded_packet_queue),
    event_packet_sink_(event_packet_sink),
    recorder_config_(recorder_config),
    event_log_(event_log) {
    if (this->recorder_config_.maximum_idle_packet_count == 0 ||
        this->recorder_config_.maximum_idle_packet_count < this->recorder_config_.lookback_packet_count) {
        throw std::invalid_argument(
            "O teto de pacotes contabilizados deve ser positivo e maior ou igual ao minimo mantido."
        );
    }
}

void EventRecorder::processDecodedFrame(const MotionState& motion_state, std::uint64_t decoded_frame_index) {
    if (this->event_packet_sink_.checkIfRecordingIsOpen()) {
        ++this->recording_read_count_;
    }

    // take* retorna o lote por valor; o lock da fila ja foi liberado
    // quando comeca a escrita no arquivo.
    switch (this->recording_state_) {
        case RecordingState::idle: {
            if (motion_state.started) {
                this->event_log_ << "EVENT_STARTED frame=" << decoded_frame_index << std::endl;
                auto encoded_packets = this->encoded_packet_queue_.takeCountedPacketsFromLookback(this->recorder_config_.lookback_packet_count);
                if (encoded_packets.empty()) {
                    this->recording_state_ = RecordingState::waiting_keyframe;
                    this->event_log_ << "EVENT_WAITING_KEYFRAME" << std::endl;
                } else {
                    const std::size_t pre_event_written_packet_count = encoded_packets.size();
                    this->openRecording();
                    this->writePackets(std::move(encoded_packets));
                    this->event_log_ << "PRE_EVENT_WRITTEN packets=" << pre_event_written_packet_count << std::endl;
                    this->recording_state_ = RecordingState::recording;
                }
            } else {
                (void)this->encoded_packet_queue_.trimCountedPackets(this->recorder_config_.lookback_packet_count, this->recorder_config_.maximum_idle_packet_count);
            }
            break;
        }
        case RecordingState::waiting_keyframe: {
            (void)this->encoded_packet_queue_.discardCountedPacketsBeforeFirstKeyframe();
            auto encoded_packets = this->encoded_packet_queue_.takeCountedPackets();
            if (!encoded_packets.empty()) {
                this->openRecording();
                this->writePackets(std::move(encoded_packets));
                this->recording_state_ = RecordingState::recording;
            } else if (motion_state.ended) {
                this->event_log_ << "EVENT_DISCARDED_NO_KEYFRAME" << std::endl;
                this->recording_state_ = RecordingState::idle;
            }
            break;
        }
        case RecordingState::recording:
            this->writePackets(this->encoded_packet_queue_.takeCountedPackets());
            break;
        case RecordingState::post_event: {
            auto encoded_packets = this->encoded_packet_queue_.takeCountedPackets();
            if (motion_state.started) {
                this->event_log_ << "EVENT_RESUMED frame=" << decoded_frame_index << std::endl;
                this->writePackets(std::move(encoded_packets));
                this->recording_state_ = RecordingState::recording;
            } else {
                const std::size_t written_packet_count = encoded_packets.size();
                this->writePackets(std::move(encoded_packets));
                this->post_remaining_packet_count_ -= std::min(this->post_remaining_packet_count_, written_packet_count);
                if (this->post_remaining_packet_count_ == 0) {
                    this->event_log_ << "POST_EVENT_EXPIRED frame=" << decoded_frame_index << std::endl;
                    this->recording_state_ = RecordingState::closing_at_keyframe;
                }
            }
            break;
        }
        case RecordingState::closing_at_keyframe: {
            if (motion_state.started) {
                this->event_log_ << "EVENT_RESUMED frame=" << decoded_frame_index << std::endl;
                this->writePackets(this->encoded_packet_queue_.takeCountedPackets());
                this->recording_state_ = RecordingState::recording;
            } else {
                bool keyframe_found = false;
                this->writePackets(
                    this->encoded_packet_queue_.takeCountedPacketsBeforeNextKeyframe(keyframe_found)
                );
                if (keyframe_found) {
                    this->finalizeRecording();
                    this->recording_state_ = RecordingState::idle;
                }
            }
            break;
        }
    }

    // Tambem cobre ended no mesmo read que abriu um arquivo apos esperar keyframe.
    if (this->recording_state_ == RecordingState::recording && motion_state.ended) {
        this->event_log_ << "EVENT_ENDED frame=" << decoded_frame_index << std::endl;
        this->post_remaining_packet_count_ = this->recorder_config_.post_event_packet_count;
        this->recording_state_ = this->post_remaining_packet_count_ == 0
            ? RecordingState::closing_at_keyframe
            : RecordingState::post_event;
    }
}

void EventRecorder::finishCapture(std::vector<AvPacketPtr> remaining_encoded_packets) {
    // Os pacotes a frente da fronteira nao passaram pelo detector, mas
    // continuam em sequencia com o evento e devem ser preservados no MP4.
    switch (this->recording_state_) {
        case RecordingState::recording:
        case RecordingState::post_event:
        case RecordingState::closing_at_keyframe: {
            const std::size_t flushed_packet_count = remaining_encoded_packets.size();
            this->writePackets(std::move(remaining_encoded_packets));
            this->finalizeRecording();
            this->event_log_ << "SHUTDOWN_FLUSHED packets=" << flushed_packet_count << std::endl;
            break;
        }
        case RecordingState::waiting_keyframe: {
            const auto first_keyframe = std::find_if(
                remaining_encoded_packets.begin(), remaining_encoded_packets.end(),
                [](const AvPacketPtr& encoded_packet) { return (encoded_packet->flags & AV_PKT_FLAG_KEY) != 0; }
            );
            remaining_encoded_packets.erase(remaining_encoded_packets.begin(), first_keyframe);
            if (!remaining_encoded_packets.empty()) {
                const std::size_t flushed_packet_count = remaining_encoded_packets.size();
                this->openRecording();
                this->writePackets(std::move(remaining_encoded_packets));
                this->finalizeRecording();
                this->event_log_ << "SHUTDOWN_FLUSHED packets=" << flushed_packet_count << std::endl;
            } else {
                this->event_log_ << "EVENT_DISCARDED_NO_KEYFRAME" << std::endl;
            }
            break;
        }
        case RecordingState::idle:
            this->event_log_ << "SHUTDOWN_DISCARDED_IDLE_PACKETS packets=" << remaining_encoded_packets.size() << std::endl;
            break;
    }
}

void EventRecorder::openRecording() {
    this->event_packet_sink_.openRecording();
    this->recording_read_count_ = 1;
    this->recording_encoded_packet_count_ = this->event_packet_sink_.getWrittenPacketCount();
    this->event_log_ << "MP4_STARTED path=" << std::filesystem::path(this->event_packet_sink_.getRecordingDescription()) << std::endl;
}

void EventRecorder::writePackets(std::vector<AvPacketPtr>&& encoded_packets_for_output) {
    this->event_packet_sink_.writePackets(std::move(encoded_packets_for_output));
    this->recording_encoded_packet_count_ = this->event_packet_sink_.getWrittenPacketCount();
}

void EventRecorder::finalizeRecording() {
    this->event_packet_sink_.finalizeRecording();
    ++this->completed_recording_count_;
    this->event_log_ << "MP4_SAVED path=" << std::filesystem::path(this->event_packet_sink_.getRecordingDescription())
              << " decoded_reads=" << this->recording_read_count_
              << " encoded_packets=" << this->recording_encoded_packet_count_ << std::endl;
}

RecordingState EventRecorder::getRecordingState() const {
    return this->recording_state_;
}

std::size_t EventRecorder::getCompletedRecordingCount() const {
    return this->completed_recording_count_;
}
