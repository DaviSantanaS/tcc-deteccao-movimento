#include "AvCodecParameters.hpp"

#include <new>
#include <stdexcept>
#include <string>

void AvCodecParametersDeleter::operator()(
    AVCodecParameters* codec_parameters
) const noexcept {
    avcodec_parameters_free(&codec_parameters);
}

AvCodecParametersPtr allocateAvCodecParameters() {
    AvCodecParametersPtr codec_parameters{avcodec_parameters_alloc()};

    if (!codec_parameters) {
        throw std::bad_alloc{};
    }

    return codec_parameters;
}
