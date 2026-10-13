#include "AvCodecParameters.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/defs.h>
#include <libavutil/mem.h>
}

int main() {
    try {
        AvCodecParametersPtr codec_parameters = allocateAvCodecParameters();
        if (!codec_parameters) {
            throw std::runtime_error("A alocacao deve devolver parametros validos.");
        }
        if (codec_parameters->codec_id != AV_CODEC_ID_NONE ||
            codec_parameters->width != 0) {
            throw std::runtime_error("A alocacao deve preservar os valores iniciais do FFmpeg.");
        }

        codec_parameters->codec_id = AV_CODEC_ID_H264;
        codec_parameters->width = 1920;
        AVCodecParameters* allocated_codec_parameters = codec_parameters.get();

        AvCodecParametersPtr moved_codec_parameters = std::move(codec_parameters);
        if (codec_parameters ||
            moved_codec_parameters.get() != allocated_codec_parameters) {
            throw std::runtime_error("A movimentacao deve transferir a posse do mesmo objeto.");
        }
        if (moved_codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            moved_codec_parameters->width != 1920) {
            throw std::runtime_error("A movimentacao deve preservar os parametros de codec.");
        }

        moved_codec_parameters.reset();
        if (moved_codec_parameters) {
            throw std::runtime_error("reset() deve deixar o proprietario vazio.");
        }

        {
            AvCodecParametersPtr scoped_codec_parameters = allocateAvCodecParameters();
            if (!scoped_codec_parameters) {
                throw std::runtime_error("A alocacao deve devolver parametros validos.");
            }
        }

        AvCodecParametersPtr empty_codec_parameters;
        empty_codec_parameters.reset();

    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
