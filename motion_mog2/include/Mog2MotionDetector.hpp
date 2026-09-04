#pragma once

#include <opencv2/core/cuda.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudafilters.hpp>

#include <cstdint>

struct MotionState {
    bool detected = false;
    bool started = false;
    bool active = false;
    bool ended = false;
    double foreground_ratio = 0.0;
};

class Mog2MotionDetector {
public:
    Mog2MotionDetector(
        int decoded_frame_width,
        int decoded_frame_height,
        double stream_fps,
        double motion_threshold_percent,
        int motion_start_frames,
        int motion_end_frames,
        double learning_rate
    );

    MotionState process(
        const cv::cuda::GpuMat& decoded_frame_gpu,
        uint64_t decoded_frame_index,
        cv::cuda::Stream& cuda_stream
    );

    uint64_t warmupFrameCount() const;
    bool isMotionActive() const;

private:
    int64_t decoded_frame_pixel_count_ = 0;
    double motion_threshold_ratio_ = 0.0;
    int motion_start_frames_ = 0;
    int motion_end_frames_ = 0;
    double learning_rate_ = 0.0;
    uint64_t warmup_frame_count_ = 0;

    cv::Ptr<cv::cuda::BackgroundSubtractorMOG2> mog2_detector_;
    cv::Ptr<cv::cuda::Filter> morphology_filter_;
    cv::cuda::GpuMat motion_mask_gpu_;

    bool motion_active_ = false;
    int motion_frame_count_ = 0;
    int idle_frame_count_ = 0;
};
