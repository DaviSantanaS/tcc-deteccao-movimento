#pragma once

#include "motion/IMotionDetector.hpp"

#include <opencv2/core.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudafilters.hpp>

namespace motion {

struct Mog2Config {
    int history{500};
    double varianceThreshold{16.0};
    bool detectShadows{false};
    double learningRate{0.01};
    double maskThreshold{200.0};
    int morphologyKernelSize{3};
};

class Mog2Detector final : public IMotionDetector {
public:
    explicit Mog2Detector(Mog2Config config = {});

    DetectionResult process(
        const cv::cuda::GpuMat& bgraFrame,
        cv::cuda::Stream& stream
    ) override;

    void reset() override;

private:
    void initialize();

    Mog2Config config_;
    cv::Ptr<cv::cuda::BackgroundSubtractorMOG2> subtractor_;
    cv::Ptr<cv::cuda::Filter> morphologyOpen_;
    cv::cuda::GpuMat mask_;
};

} // namespace motion
