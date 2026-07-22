#pragma once

#include "motion/IMotionDetector.hpp"

#include <opencv2/core.hpp>
#include <opencv2/cudafilters.hpp>

namespace motion {

struct FrameDifferenceConfig {
    int differenceThreshold{30};
    int gaussianKernelSize{5};
    double gaussianSigma{0.0};
    int morphologyKernelSize{3};
};

class FrameDifferenceDetector final : public IMotionDetector {
public:
    explicit FrameDifferenceDetector(FrameDifferenceConfig config = {});

    DetectionResult process(
        const cv::cuda::GpuMat& bgraFrame,
        cv::cuda::Stream& stream
    ) override;

    void reset() override;

private:
    FrameDifferenceConfig config_;
    cv::Ptr<cv::cuda::Filter> gaussianBlur_;
    cv::Ptr<cv::cuda::Filter> morphologyOpen_;
    cv::cuda::GpuMat gray_;
    cv::cuda::GpuMat previousGray_;
    cv::cuda::GpuMat difference_;
    cv::cuda::GpuMat blurred_;
    cv::cuda::GpuMat mask_;
};

} // namespace motion
