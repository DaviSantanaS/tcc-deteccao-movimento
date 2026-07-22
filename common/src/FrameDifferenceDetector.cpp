#include "motion/FrameDifferenceDetector.hpp"

#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudaimgproc.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace motion {

FrameDifferenceDetector::FrameDifferenceDetector(FrameDifferenceConfig config)
    : config_(std::move(config))
{
    config_.gaussianKernelSize = std::max(
        3,
        config_.gaussianKernelSize | 1
    );

    if (config_.morphologyKernelSize <= 0 ||
        config_.morphologyKernelSize % 2 == 0) {
        throw std::invalid_argument(
            "FrameDifferenceConfig.morphologyKernelSize deve ser positivo e impar"
        );
    }

    gaussianBlur_ = cv::cuda::createGaussianFilter(
        CV_8UC1,
        CV_8UC1,
        cv::Size(
            config_.gaussianKernelSize,
            config_.gaussianKernelSize
        ),
        config_.gaussianSigma
    );

    const cv::Mat kernel = cv::getStructuringElement(
        cv::MORPH_RECT,
        cv::Size(
            config_.morphologyKernelSize,
            config_.morphologyKernelSize
        )
    );

    morphologyOpen_ = cv::cuda::createMorphologyFilter(
        cv::MORPH_OPEN,
        CV_8UC1,
        kernel
    );
}

DetectionResult FrameDifferenceDetector::process(
    const cv::cuda::GpuMat& bgraFrame,
    cv::cuda::Stream& stream
)
{
    if (bgraFrame.empty()) {
        return {};
    }

    if (bgraFrame.type() != CV_8UC4) {
        throw std::invalid_argument(
            "FrameDifferenceDetector espera frame CV_8UC4 em formato BGRA"
        );
    }

    cv::cuda::cvtColor(
        bgraFrame,
        gray_,
        cv::COLOR_BGRA2GRAY,
        0,
        stream
    );

    if (previousGray_.empty()) {
        gray_.copyTo(previousGray_, stream);
        stream.waitForCompletion();
        return {};
    }

    cv::cuda::absdiff(
        gray_,
        previousGray_,
        difference_,
        stream
    );
    gaussianBlur_->apply(difference_, blurred_, stream);
    cv::cuda::threshold(
        blurred_,
        mask_,
        config_.differenceThreshold,
        255.0,
        cv::THRESH_BINARY,
        stream
    );
    morphologyOpen_->apply(mask_, mask_, stream);
    gray_.copyTo(previousGray_, stream);

    stream.waitForCompletion();
    const std::int64_t activePixels = cv::cuda::countNonZero(mask_);

    return DetectionResult{true, activePixels};
}

void FrameDifferenceDetector::reset()
{
    previousGray_.release();
}

} // namespace motion
