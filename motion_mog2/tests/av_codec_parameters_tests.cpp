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

        AvCodecParametersPtr cloned_default_codec_parameters =
            cloneAvCodecParameters(*codec_parameters);
        if (!cloned_default_codec_parameters ||
            cloned_default_codec_parameters.get() == codec_parameters.get() ||
            cloned_default_codec_parameters->codec_id != AV_CODEC_ID_NONE ||
            cloned_default_codec_parameters->width != 0 ||
            cloned_default_codec_parameters->extradata != nullptr ||
            cloned_default_codec_parameters->extradata_size != 0) {
            throw std::runtime_error("O clone sem extradata deve ser independente e preservar os valores.");
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

        AvCodecParametersPtr source_codec_parameters = allocateAvCodecParameters();
        source_codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
        source_codec_parameters->codec_id = AV_CODEC_ID_H264;
        source_codec_parameters->width = 1920;
        source_codec_parameters->height = 1080;

        constexpr uint8_t expected_codec_extra_data[] = {0x01, 0x02, 0x03};
        constexpr int codec_extra_data_size =
            static_cast<int>(sizeof(expected_codec_extra_data));
        source_codec_parameters->extradata = static_cast<uint8_t*>(
            av_mallocz(codec_extra_data_size + AV_INPUT_BUFFER_PADDING_SIZE)
        );
        if (!source_codec_parameters->extradata) {
            throw std::runtime_error("Nao foi possivel alocar a extradata do teste.");
        }
        source_codec_parameters->extradata_size = codec_extra_data_size;
        std::memcpy(
            source_codec_parameters->extradata,
            expected_codec_extra_data,
            codec_extra_data_size
        );

        AvCodecParametersPtr cloned_codec_parameters =
            cloneAvCodecParameters(*source_codec_parameters);
        if (!cloned_codec_parameters ||
            cloned_codec_parameters.get() == source_codec_parameters.get() ||
            cloned_codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
            cloned_codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            cloned_codec_parameters->width != 1920 ||
            cloned_codec_parameters->height != 1080) {
            throw std::runtime_error("O clone deve preservar os parametros de video em outro objeto.");
        }
        if (!cloned_codec_parameters->extradata ||
            cloned_codec_parameters->extradata == source_codec_parameters->extradata ||
            cloned_codec_parameters->extradata_size != codec_extra_data_size ||
            std::memcmp(
                cloned_codec_parameters->extradata,
                expected_codec_extra_data,
                codec_extra_data_size
            ) != 0) {
            throw std::runtime_error("O clone deve possuir uma copia independente da extradata.");
        }

        source_codec_parameters->width = 640;
        source_codec_parameters->extradata[0] = 0x09;
        if (cloned_codec_parameters->width != 1920 ||
            cloned_codec_parameters->extradata[0] != 0x01) {
            throw std::runtime_error("Alterar a origem nao deve modificar o clone.");
        }

        source_codec_parameters.reset();
        if (cloned_codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            cloned_codec_parameters->width != 1920 ||
            cloned_codec_parameters->height != 1080 ||
            cloned_codec_parameters->extradata_size != codec_extra_data_size ||
            std::memcmp(
                cloned_codec_parameters->extradata,
                expected_codec_extra_data,
                codec_extra_data_size
            ) != 0) {
            throw std::runtime_error("O clone deve continuar valido depois de liberar a origem.");
        }

        std::cout << "PASS: alocacao, ownership e clone independente com e sem extradata.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
