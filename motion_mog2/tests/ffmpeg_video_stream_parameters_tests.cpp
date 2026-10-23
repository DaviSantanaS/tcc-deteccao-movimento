#include "FfmpegRawVideoSource.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Uso: ffmpeg_video_stream_parameters_tests <arquivo-local-h264>\n";
        return 2;
    }

    try {
        std::shared_ptr<const VideoStreamParameters> retained_video_stream_parameters;
        std::weak_ptr<const VideoStreamParameters> video_stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> codec_parameters_lifetime;
        int original_width = 0;
        int original_height = 0;
        AVRational original_packet_timestamp_time_base{0, 1};
        std::vector<std::uint8_t> original_codec_extra_data;

        {
            FfmpegRawVideoSource video_source(argv[1]);
            const auto first_video_stream_parameters = video_source.videoStreamParameters();
            const auto second_video_stream_parameters = video_source.videoStreamParameters();
            if (!first_video_stream_parameters ||
                !first_video_stream_parameters->codec_parameters ||
                first_video_stream_parameters.get() != second_video_stream_parameters.get()) {
                throw std::runtime_error("O getter deve compartilhar uma unica configuracao valida.");
            }

            const auto& codec_parameters = first_video_stream_parameters->codec_parameters;
            const auto video_format = video_source.format();
            if (codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
                codec_parameters->codec_id != AV_CODEC_ID_H264 ||
                codec_parameters->width <= 0 || codec_parameters->height <= 0 ||
                codec_parameters->width != video_format.width ||
                codec_parameters->height != video_format.height ||
                codec_parameters->extradata == nullptr || codec_parameters->extradata_size <= 0) {
                throw std::runtime_error("A configuracao deve preservar codec, resolucao e extradata H.264.");
            }

            const AVRational packet_timestamp_time_base =
                first_video_stream_parameters->packet_timestamp_time_base;
            if (packet_timestamp_time_base.num <= 0 || packet_timestamp_time_base.den <= 0) {
                throw std::runtime_error("A configuracao deve preservar uma base de tempo positiva.");
            }

            original_width = codec_parameters->width;
            original_height = codec_parameters->height;
            original_packet_timestamp_time_base = packet_timestamp_time_base;
            original_codec_extra_data.assign(
                codec_parameters->extradata,
                codec_parameters->extradata + codec_parameters->extradata_size
            );
            retained_video_stream_parameters = first_video_stream_parameters;
            video_stream_parameters_lifetime = first_video_stream_parameters;
            codec_parameters_lifetime = codec_parameters;

            if (!std::isfinite(video_format.fps) || video_format.fps <= 0.0) {
                throw std::runtime_error("O FFmpeg deve fornecer um FPS inicial finito e positivo.");
            }
            const std::array<double, 5> invalid_frame_rates{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(),
                0.0,
                -1.0
            };
            auto decoder_video_format = video_format;
            decoder_video_format.width += 16;
            decoder_video_format.valid = false;
            for (const double invalid_fps : invalid_frame_rates) {
                decoder_video_format.fps = invalid_fps;
                video_source.updateFormat(decoder_video_format);
                const auto updated_format = video_source.format();
                if (!std::isfinite(updated_format.fps) || updated_format.fps != video_format.fps ||
                    updated_format.width != decoder_video_format.width ||
                    updated_format.height != decoder_video_format.height || !updated_format.valid) {
                    throw std::runtime_error("Um FPS invalido deve preservar o anterior sem impedir a atualizacao do formato.");
                }
            }

            decoder_video_format.fps = 30.0;
            video_source.updateFormat(decoder_video_format);
            if (video_source.format().fps != 30.0) {
                throw std::runtime_error("Um novo FPS finito e positivo deve ser aceito.");
            }
            decoder_video_format.fps = std::numeric_limits<double>::quiet_NaN();
            video_source.updateFormat(decoder_video_format);
            if (video_source.format().fps != 30.0) {
                throw std::runtime_error("O fallback deve preservar o ultimo FPS valido, nao somente o inicial.");
            }
        }

        if (video_stream_parameters_lifetime.expired() || codec_parameters_lifetime.expired()) {
            throw std::runtime_error("A configuracao deve continuar viva depois de destruir a fonte.");
        }

        const auto& retained_codec_parameters = retained_video_stream_parameters->codec_parameters;
        if (retained_codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
            retained_codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            retained_codec_parameters->width != original_width ||
            retained_codec_parameters->height != original_height ||
            retained_video_stream_parameters->packet_timestamp_time_base.num !=
                original_packet_timestamp_time_base.num ||
            retained_video_stream_parameters->packet_timestamp_time_base.den !=
                original_packet_timestamp_time_base.den ||
            retained_codec_parameters->extradata == nullptr ||
            static_cast<std::size_t>(retained_codec_parameters->extradata_size) !=
                original_codec_extra_data.size() ||
            std::memcmp(retained_codec_parameters->extradata,
                        original_codec_extra_data.data(), original_codec_extra_data.size()) != 0) {
            throw std::runtime_error("Destruir a fonte nao deve invalidar os parametros preservados.");
        }

        retained_video_stream_parameters.reset();
        if (!video_stream_parameters_lifetime.expired() || !codec_parameters_lifetime.expired()) {
            throw std::runtime_error("A ultima referencia deve liberar a configuracao e seus parametros.");
        }

        std::cout << "PASS: configuracao unica da fonte, preservada apos seu fechamento e liberada ao final.\n";
        std::cout << "PASS: updateFormat preserva FPS validos, aceita uma nova taxa e atualiza os demais campos.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
