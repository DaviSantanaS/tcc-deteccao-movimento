#include "Mog2MotionDetector.hpp"

#include <opencv2/cudaarithm.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
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
        static_cast<std::int64_t>(decoded_frame_width) *
        static_cast<std::int64_t>(decoded_frame_height)
    ),
    motion_threshold_ratio_(motion_threshold_percent / 100.0),
    motion_start_frame_count_(motion_start_frame_count),
    motion_end_frame_count_(motion_end_frame_count),
    learning_rate_(learning_rate),
    warmup_frame_count_(
        static_cast<std::uint64_t>(stream_fps * 1.0 + 0.5)
    ) {
    if (motion_threshold_percent <= 0.0 || motion_threshold_percent > 100.0) {
        throw std::runtime_error(
            "motion_threshold_percent deve estar no intervalo (0, 100]. Valor: " +
            std::to_string(motion_threshold_percent)
        );
    }

    const int mog2_history_frame_count = 500;
    const double mog2_variance_threshold = 16.0;

    this->mog2_detector_ = cv::cuda::createBackgroundSubtractorMOG2(
        mog2_history_frame_count,
        mog2_variance_threshold,
        false
    );
    this->mog2_detector_->setDetectShadows(false);

    cv::Mat morphology_kernel =
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));

    this->morphology_filter_ = cv::cuda::createMorphologyFilter(
        cv::MORPH_OPEN,
        CV_8UC1,
        morphology_kernel
    );
}

MotionState Mog2MotionDetector::detectMotion(
    const cv::cuda::GpuMat& decoded_frame_gpu,
    std::uint64_t decoded_frame_index,
    cv::cuda::Stream& cuda_stream
) {
    this->mog2_detector_->apply(
        decoded_frame_gpu,
        this->motion_mask_gpu_,
        this->learning_rate_,
        cuda_stream
    );

    cv::cuda::threshold(
        this->motion_mask_gpu_,
        this->motion_mask_gpu_,
        200,
        255,
        cv::THRESH_BINARY,
        cuda_stream
    );

    this->morphology_filter_->apply(
        this->motion_mask_gpu_,
        this->motion_mask_gpu_,
        cuda_stream
    );

    cuda_stream.waitForCompletion();

    const std::int64_t foreground_pixel_count =
        cv::cuda::countNonZero(this->motion_mask_gpu_);

    MotionState motion_state;

    if (decoded_frame_index < this->warmup_frame_count_) {
        this->consecutive_motion_frame_count_ = 0;
        this->consecutive_idle_frame_count_ = 0;
        motion_state.active = this->motion_active_;
        return motion_state;
    }

    motion_state.foreground_ratio =
        static_cast<double>(foreground_pixel_count) /
        static_cast<double>(this->decoded_frame_pixel_count_);

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

std::uint64_t Mog2MotionDetector::warmupFrameCount() const {
    return this->warmup_frame_count_;
}

bool Mog2MotionDetector::isMotionActive() const {
    return this->motion_active_;
}
