#include "motion/Mog2Detector.hpp"

#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudaimgproc.hpp>

#include <stdexcept>
#include <utility>

namespace motion {

Mog2Detector::Mog2Detector(Mog2Config config)
    : config_(std::move(config))
{
    if (config_.history <= 0) {
        throw std::invalid_argument("Mog2Config.history deve ser maior que zero");
    }

    if (config_.morphologyKernelSize <= 0 ||
        config_.morphologyKernelSize % 2 == 0) {
        throw std::invalid_argument(
            "Mog2Config.morphologyKernelSize deve ser positivo e impar"
        );
    }

    initialize();
}

void Mog2Detector::initialize()
{
    subtractor_ = cv::cuda::createBackgroundSubtractorMOG2(
        config_.history,
        config_.varianceThreshold,
        config_.detectShadows
    );

    subtractor_->setDetectShadows(config_.detectShadows);

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

DetectionResult Mog2Detector::process(
    const cv::cuda::GpuMat& bgraFrame,
    cv::cuda::Stream& stream
)
{
    if (bgraFrame.empty()) {
        return {};
    }

    if (bgraFrame.type() != CV_8UC4) {
        throw std::invalid_argument(
            "Mog2Detector espera frame CV_8UC4 em formato BGRA"
        );
    }

    subtractor_->apply(
        bgraFrame,
        mask_,
        config_.learningRate,
        stream
    );

    cv::cuda::threshold(
        mask_,
        mask_,
        config_.maskThreshold,
        255.0,
        cv::THRESH_BINARY,
        stream
    );

    morphologyOpen_->apply(mask_, mask_, stream);

    stream.waitForCompletion();

    return DetectionResult{
        true,
        cv::cuda::countNonZero(mask_)
    };
}

void Mog2Detector::reset()
{
    mask_.release();
    initialize();
}

} // namespace motion
