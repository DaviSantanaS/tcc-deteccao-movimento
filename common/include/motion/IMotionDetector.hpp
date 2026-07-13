#pragma once

#include <cstdint>
#include <opencv2/core/cuda.hpp>

namespace motion {

struct DetectionResult {
    bool valid{false};
    std::int64_t activePixels{0};
};

class IMotionDetector {
public:
    virtual ~IMotionDetector() = default;

    virtual void reset() = 0;

    // Pré-condição: frame CV_8UC4 em formato BGRA, residente na GPU.
    virtual DetectionResult process(
        const cv::cuda::GpuMat& bgraFrame,
        cv::cuda::Stream& stream
    ) = 0;
};

} // namespace motion
