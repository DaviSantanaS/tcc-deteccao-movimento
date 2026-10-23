#include "Mog2MotionDetector.hpp"

#include <opencv2/cudaarithm.hpp>
#include <opencv2/imgproc.hpp>

#include <stdexcept>
#include <string>

Mog2MotionDetector::Mog2MotionDetector(
    int decoded_frame_width,
    int decoded_frame_height,
    double stream_fps,
    double motion_threshold_percent,
    int motion_start_frame_count,
    int motion_end_frame_count,
    double learning_rate
) :
    decoded_frame_pixel_count_(
        static_cast<int64_t>(decoded_frame_width) *
        static_cast<int64_t>(decoded_frame_height)
    ),
    motion_threshold_ratio_(motion_threshold_percent / 100.0),
    motion_start_frame_count_(motion_start_frame_count),
    motion_end_frame_count_(motion_end_frame_count),
    learning_rate_(learning_rate),
    warmup_frame_count_(
        static_cast<uint64_t>(stream_fps * 1.0 + 0.5)
    ) {
    if (motion_threshold_percent <= 0.0 || motion_threshold_percent > 100.0) {
        throw std::runtime_error(
            "motion_threshold_percent deve estar no intervalo (0, 100]. Valor: " +
            std::to_string(motion_threshold_percent)
        );
    }

    const int mog2_history_frame_count = 500;
    const double mog2_variance_threshold = 16.0;

    mog2_detector_ = cv::cuda::createBackgroundSubtractorMOG2(
        mog2_history_frame_count,
        mog2_variance_threshold,
        false
    );
    mog2_detector_->setDetectShadows(false);

    cv::Mat morphology_kernel =
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));

    morphology_filter_ = cv::cuda::createMorphologyFilter(
        cv::MORPH_OPEN,
        CV_8UC1,
        morphology_kernel
    );
}

MotionState Mog2MotionDetector::detectMotion(
    const cv::cuda::GpuMat& decoded_frame_gpu,
    uint64_t decoded_frame_index,
    cv::cuda::Stream& cuda_stream
) {
    mog2_detector_->apply(
        decoded_frame_gpu,
        motion_mask_gpu_,
        learning_rate_,
        cuda_stream
    );

    cv::cuda::threshold(
        motion_mask_gpu_,
        motion_mask_gpu_,
        200,
        255,
        cv::THRESH_BINARY,
        cuda_stream
    );

    morphology_filter_->apply(
        motion_mask_gpu_,
        motion_mask_gpu_,
        cuda_stream
    );

    cuda_stream.waitForCompletion();

    const int64_t foreground_pixel_count =
        cv::cuda::countNonZero(motion_mask_gpu_);

    MotionState motion_state;

    if (decoded_frame_index < warmup_frame_count_) {
        consecutive_motion_frame_count_ = 0;
        consecutive_idle_frame_count_ = 0;
        motion_state.active = motion_active_;
        return motion_state;
    }

    motion_state.foreground_ratio =
        static_cast<double>(foreground_pixel_count) /
        static_cast<double>(decoded_frame_pixel_count_);

    motion_state.detected =
        motion_state.foreground_ratio >= motion_threshold_ratio_;

    if (!motion_active_) {
        if (motion_state.detected) {
            if (++consecutive_motion_frame_count_ >= motion_start_frame_count_) {
                motion_active_ = true;
                motion_state.started = true;
                consecutive_motion_frame_count_ = 0;
                consecutive_idle_frame_count_ = 0;
            }
        } else {
            consecutive_motion_frame_count_ = 0;
        }
    } else {
        if (!motion_state.detected) {
            if (++consecutive_idle_frame_count_ >= motion_end_frame_count_) {
                motion_active_ = false;
                motion_state.ended = true;
                consecutive_idle_frame_count_ = 0;
                consecutive_motion_frame_count_ = 0;
            }
        } else {
            consecutive_idle_frame_count_ = 0;
        }
    }

    motion_state.active = motion_active_;
    return motion_state;
}

uint64_t Mog2MotionDetector::warmupFrameCount() const {
    return warmup_frame_count_;
}

bool Mog2MotionDetector::isMotionActive() const {
    return motion_active_;
}
