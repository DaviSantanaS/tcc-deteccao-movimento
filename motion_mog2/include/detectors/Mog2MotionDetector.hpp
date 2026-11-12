#pragma once

#include "detectors/MotionHysteresis.hpp"

#include <opencv2/core/cuda.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudafilters.hpp>

#include <cstdint>

class Mog2MotionDetector {
public:
    Mog2MotionDetector(
        int decoded_frame_width,
        int decoded_frame_height,
        double stream_fps,
        double motion_threshold_percent,
        int motion_start_frame_count,
        int motion_end_frame_count,
        double learning_rate
    );

    MotionState detectMotion(
        const cv::cuda::GpuMat& decoded_frame_gpu,
        std::uint64_t decoded_frame_index,
        cv::cuda::Stream& cuda_stream
    );

    std::uint64_t getWarmupFrameCount() const;
    bool checkIfMotionIsActive() const;

private:
    std::int64_t decoded_frame_pixel_count_ = 0;
    double learning_rate_ = 0.0;
    MotionHysteresis motion_hysteresis_;

    cv::Ptr<cv::cuda::BackgroundSubtractorMOG2> mog2_detector_;
    cv::Ptr<cv::cuda::Filter> morphology_filter_;
    cv::cuda::GpuMat motion_mask_gpu_;
};
