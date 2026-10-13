#include "AvCodecParameters.hpp"

#include <new>
#include <stdexcept>
#include <string>

void AvCodecParametersDeleter::operator()(
    AVCodecParameters* codec_parameters
) const noexcept {
    avcodec_parameters_free(&codec_parameters);
}
