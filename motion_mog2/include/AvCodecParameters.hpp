#pragma once

extern "C" {
#include <libavcodec/codec_par.h>
}

#include <memory>

struct AvCodecParametersDeleter {
    void operator()(AVCodecParameters* codec_parameters) const noexcept;
};

using AvCodecParametersPtr =
    std::unique_ptr<AVCodecParameters, AvCodecParametersDeleter>;

[[nodiscard]] AvCodecParametersPtr allocateAvCodecParameters();

[[nodiscard]] AvCodecParametersPtr cloneAvCodecParameters(
    const AVCodecParameters& source_codec_parameters
);
