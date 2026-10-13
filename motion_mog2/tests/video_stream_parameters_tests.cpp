#include "VideoStreamParameters.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        std::shared_ptr<const VideoStreamParameters> retained_video_stream_parameters;
        std::weak_ptr<const VideoStreamParameters> video_stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> codec_parameters_lifetime;

        {
            AvCodecParametersPtr source_codec_parameters = allocateAvCodecParameters();
            source_codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            source_codec_parameters->codec_id = AV_CODEC_ID_H264;
            source_codec_parameters->width = 1920;
            source_codec_parameters->height = 1080;

            auto video_stream_parameters = std::make_shared<const VideoStreamParameters>(
                *source_codec_parameters,
                AVRational{1, 90000}
            );
            if (!video_stream_parameters->codec_parameters ||
                video_stream_parameters->codec_parameters.get() == source_codec_parameters.get() ||
                video_stream_parameters->codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
                video_stream_parameters->codec_parameters->codec_id != AV_CODEC_ID_H264 ||
                video_stream_parameters->codec_parameters->width != 1920 ||
                video_stream_parameters->codec_parameters->height != 1080) {
                throw std::runtime_error("A configuracao deve preservar uma copia independente dos parametros.");
            }
            if (video_stream_parameters->packet_timestamp_time_base.num != 1 ||
                video_stream_parameters->packet_timestamp_time_base.den != 90000) {
                throw std::runtime_error("A configuracao deve preservar a base de tempo dos pacotes.");
            }

            retained_video_stream_parameters = video_stream_parameters;
            video_stream_parameters_lifetime = video_stream_parameters;
            codec_parameters_lifetime = video_stream_parameters->codec_parameters;

            source_codec_parameters->width = 640;
            source_codec_parameters->height = 360;
            if (video_stream_parameters->codec_parameters->width != 1920 ||
                video_stream_parameters->codec_parameters->height != 1080) {
                throw std::runtime_error("Alterar a origem nao deve modificar a configuracao preservada.");
            }

            source_codec_parameters.reset();
            if (video_stream_parameters->codec_parameters->codec_id != AV_CODEC_ID_H264 ||
                video_stream_parameters->codec_parameters->width != 1920 ||
                video_stream_parameters->codec_parameters->height != 1080) {
                throw std::runtime_error("A configuracao deve continuar valida depois de liberar a origem.");
            }
        }

        if (video_stream_parameters_lifetime.expired() ||
            codec_parameters_lifetime.expired() ||
            retained_video_stream_parameters->codec_parameters->width != 1920) {
            throw std::runtime_error("A referencia compartilhada deve manter a configuracao viva.");
        }

        retained_video_stream_parameters.reset();
        if (!video_stream_parameters_lifetime.expired() ||
            !codec_parameters_lifetime.expired()) {
            throw std::runtime_error("A ultima referencia deve liberar a configuracao e seus parametros.");
        }

        AvCodecParametersPtr source_codec_parameters = allocateAvCodecParameters();
        constexpr AVRational invalid_packet_timestamp_time_bases[] = {
            {0, 90000}, {-1, 90000}, {1, 0}, {1, -90000}
        };
        for (const AVRational invalid_packet_timestamp_time_base : invalid_packet_timestamp_time_bases) {
            bool rejected_invalid_time_base = false;
            try {
                VideoStreamParameters invalid_video_stream_parameters(
                    *source_codec_parameters,
                    invalid_packet_timestamp_time_base
                );
            } catch (const std::invalid_argument&) {
                rejected_invalid_time_base = true;
            }
            if (!rejected_invalid_time_base) {
                throw std::runtime_error("O construtor deve rejeitar bases de tempo nao positivas.");
            }
        }

        std::cout << "PASS: copia independente, base de tempo e posse compartilhada do stream.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
