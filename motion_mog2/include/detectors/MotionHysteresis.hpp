#pragma once

#include <cstdint>

struct MotionState {
    bool detected = false;
    bool started = false;
    bool active = false;
    bool ended = false;
    double foreground_ratio = 0.0;
};

class MotionHysteresis {
public:
    MotionHysteresis(
        double motion_threshold_ratio,
        int motion_start_frame_count,
        int motion_end_frame_count,
        std::uint64_t warmup_frame_count
    );

    MotionState update(double foreground_ratio, std::uint64_t decoded_frame_index);
    bool checkIfMotionIsActive() const;
    std::uint64_t getWarmupFrameCount() const;

private:
    double motion_threshold_ratio_ = 0.0;
    int motion_start_frame_count_ = 0;
    int motion_end_frame_count_ = 0;
    std::uint64_t warmup_frame_count_ = 0;
    bool motion_active_ = false;
    int consecutive_motion_frame_count_ = 0;
    int consecutive_idle_frame_count_ = 0;
};
