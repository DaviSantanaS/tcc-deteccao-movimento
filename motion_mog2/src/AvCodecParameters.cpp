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

AvCodecParametersPtr cloneAvCodecParameters(
    const AVCodecParameters& source_codec_parameters
) {
    AvCodecParametersPtr cloned_codec_parameters = allocateAvCodecParameters();

    const int codec_parameters_copy_result = avcodec_parameters_copy(
        cloned_codec_parameters.get(),
        &source_codec_parameters
    );

    if (codec_parameters_copy_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel copiar os parametros de codec: erro " +
            std::to_string(codec_parameters_copy_result)
        );
    }

    return cloned_codec_parameters;
}
