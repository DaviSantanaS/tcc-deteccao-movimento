#include "detectors/MotionHysteresis.hpp"

#include <cstdint>

MotionHysteresis::MotionHysteresis(
    double motion_threshold_ratio,
    int motion_start_frame_count,
    int motion_end_frame_count,
    std::uint64_t warmup_frame_count
) :
    motion_threshold_ratio_(motion_threshold_ratio),
    motion_start_frame_count_(motion_start_frame_count),
    motion_end_frame_count_(motion_end_frame_count),
    warmup_frame_count_(warmup_frame_count) {
}

MotionState MotionHysteresis::update(double foreground_ratio, std::uint64_t decoded_frame_index) {
    MotionState motion_state;

    if (decoded_frame_index < this->warmup_frame_count_) {
        this->consecutive_motion_frame_count_ = 0;
        this->consecutive_idle_frame_count_ = 0;
        motion_state.active = this->motion_active_;
        return motion_state;
    }

    motion_state.foreground_ratio = foreground_ratio;
    motion_state.detected =
        motion_state.foreground_ratio >= this->motion_threshold_ratio_;

    if (!this->motion_active_) {
        if (motion_state.detected) {
            if (++this->consecutive_motion_frame_count_ >= this->motion_start_frame_count_) {
                this->motion_active_ = true;
                motion_state.started = true;
                this->consecutive_motion_frame_count_ = 0;
                this->consecutive_idle_frame_count_ = 0;
            }
        } else {
            this->consecutive_motion_frame_count_ = 0;
        }
    } else {
        if (!motion_state.detected) {
            if (++this->consecutive_idle_frame_count_ >= this->motion_end_frame_count_) {
                this->motion_active_ = false;
                motion_state.ended = true;
                this->consecutive_idle_frame_count_ = 0;
                this->consecutive_motion_frame_count_ = 0;
            }
        } else {
            this->consecutive_idle_frame_count_ = 0;
        }
    }

    motion_state.active = this->motion_active_;
    return motion_state;
}

bool MotionHysteresis::checkIfMotionIsActive() const {
    return this->motion_active_;
}

std::uint64_t MotionHysteresis::getWarmupFrameCount() const {
    return this->warmup_frame_count_;
}
