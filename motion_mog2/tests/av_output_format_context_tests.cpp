#include "AvCodecParameters.hpp"
#include "AvOutputFormatContext.hpp"
#include "VideoStreamParameters.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/defs.h>
#include <libavutil/mem.h>
}

int main() {
    try {
        AvOutputFormatContextPtr output_context = allocateMp4OutputContext();
        if (!output_context || !output_context->oformat ||
            !output_context->oformat->name ||
            std::strcmp(output_context->oformat->name, "mp4") != 0 ||
            output_context->iformat != nullptr || output_context->pb != nullptr ||
            output_context->nb_streams != 0) {
            throw std::runtime_error("A alocacao deve configurar MP4 sem abrir arquivo nem criar streams.");
        }

        AvOutputFormatContextPtr independent_output_context = allocateMp4OutputContext();
        if (!independent_output_context ||
            independent_output_context.get() == output_context.get()) {
            throw std::runtime_error("Duas alocacoes devem possuir contextos independentes.");
        }
        AVFormatContext* allocated_output_context = output_context.get();
        AvOutputFormatContextPtr moved_output_context = std::move(output_context);
        if (output_context || moved_output_context.get() != allocated_output_context ||
            std::strcmp(moved_output_context->oformat->name, "mp4") != 0 ||
            moved_output_context->pb != nullptr || moved_output_context->nb_streams != 0) {
            throw std::runtime_error("O movimento deve transferir a posse do mesmo contexto MP4.");
        }

        moved_output_context.reset();
        if (moved_output_context || !independent_output_context->oformat ||
            !independent_output_context->oformat->name ||
            std::strcmp(independent_output_context->oformat->name, "mp4") != 0 ||
            independent_output_context->pb != nullptr ||
            independent_output_context->nb_streams != 0) {
            throw std::runtime_error("reset() deve esvaziar seu proprietario sem invalidar o contexto independente.");
        }

        {
            AvOutputFormatContextPtr scoped_output_context = allocateMp4OutputContext();
            if (!scoped_output_context) {
                throw std::runtime_error("O contexto de escopo deve ser valido antes da destruicao automatica.");
            }
        }
        AvOutputFormatContextPtr empty_output_context;
        empty_output_context.reset();
        AvOutputFormatContextDeleter{}(nullptr);

        std::cout << "PASS: contexto MP4 alocado sem arquivo aberto nem streams.\n";
        std::cout << "PASS: alocacoes independentes e transferencia de posse sem alterar o endereco.\n";
        std::cout << "PASS: reset, destruicao por escopo e proprietario vazio sem erros.\n";

        const auto expect_rejection_without_stream_changes = [](
            AVFormatContext& context,
            const VideoStreamParameters& parameters
        ) {
            const unsigned int previous_stream_count = context.nb_streams;
            AVStream** previous_streams = context.streams;
            bool rejected = false;
            try {
                (void)createMp4VideoStream(context, parameters);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (!rejected || context.nb_streams != previous_stream_count ||
                context.streams != previous_streams) {
                throw std::runtime_error("Uma entrada invalida deve ser rejeitada sem alterar as streams.");
            }
        };

        constexpr unsigned char expected_codec_extra_data[]{0x01, 0x02, 0x03};
        constexpr unsigned int source_codec_tag = 1234;
        AvOutputFormatContextPtr video_output_context = allocateMp4OutputContext();
        {
            AvCodecParametersPtr source_codec_parameters = allocateAvCodecParameters();
            source_codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            source_codec_parameters->codec_id = AV_CODEC_ID_H264;
            source_codec_parameters->codec_tag = source_codec_tag;
            source_codec_parameters->width = 1920;
            source_codec_parameters->height = 1080;
            source_codec_parameters->extradata = static_cast<unsigned char*>(
                av_mallocz(sizeof(expected_codec_extra_data) + AV_INPUT_BUFFER_PADDING_SIZE)
            );
            if (!source_codec_parameters->extradata) {
                throw std::bad_alloc{};
            }
            source_codec_parameters->extradata_size = sizeof(expected_codec_extra_data);
            std::memcpy(source_codec_parameters->extradata, expected_codec_extra_data,
                        sizeof(expected_codec_extra_data));
            VideoStreamParameters video_stream_parameters{
                *source_codec_parameters, AVRational{1, 90000}
            };

            AVStream& video_output_stream =
                createMp4VideoStream(*video_output_context, video_stream_parameters);
            if (video_output_context->nb_streams != 1 || !video_output_context->streams ||
                video_output_context->streams[0] != &video_output_stream ||
                video_output_stream.index != 0 || video_output_context->pb != nullptr ||
                !video_output_stream.codecpar ||
                video_output_stream.codecpar == video_stream_parameters.codec_parameters.get() ||
                video_output_stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                video_output_stream.codecpar->codec_id != AV_CODEC_ID_H264 ||
                video_output_stream.codecpar->width != 1920 ||
                video_output_stream.codecpar->height != 1080 ||
                video_output_stream.codecpar->codec_tag != 0 ||
                video_output_stream.time_base.num != 1 || video_output_stream.time_base.den != 90000) {
                throw std::runtime_error("A stream deve pertencer ao contexto MP4 e conter os parametros copiados.");
            }
            if (!video_output_stream.codecpar->extradata ||
                video_output_stream.codecpar->extradata == video_stream_parameters.codec_parameters->extradata ||
                video_output_stream.codecpar->extradata_size != sizeof(expected_codec_extra_data) ||
                std::memcmp(video_output_stream.codecpar->extradata, expected_codec_extra_data,
                            sizeof(expected_codec_extra_data)) != 0 ||
                video_stream_parameters.codec_parameters->codec_tag != source_codec_tag ||
                std::memcmp(video_stream_parameters.codec_parameters->extradata, expected_codec_extra_data,
                            sizeof(expected_codec_extra_data)) != 0) {
                throw std::runtime_error("A copia deve duplicar a extradata sem modificar os parametros da origem.");
            }

            const AVCodecParameters* output_codec_parameters = video_output_stream.codecpar;
            const unsigned char* output_codec_extra_data = video_output_stream.codecpar->extradata;
            expect_rejection_without_stream_changes(*video_output_context, video_stream_parameters);
            if (video_output_stream.codecpar != output_codec_parameters ||
                video_output_stream.codecpar->extradata != output_codec_extra_data ||
                video_output_stream.codecpar->codec_tag != 0 ||
                video_output_stream.time_base.num != 1 || video_output_stream.time_base.den != 90000) {
                throw std::runtime_error("Rejeitar uma segunda stream deve preservar a primeira.");
            }
        }
        const AVStream& surviving_output_stream = *video_output_context->streams[0];
        if (surviving_output_stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            surviving_output_stream.codecpar->codec_id != AV_CODEC_ID_H264 ||
            surviving_output_stream.codecpar->width != 1920 ||
            surviving_output_stream.codecpar->height != 1080 ||
            surviving_output_stream.codecpar->codec_tag != 0 ||
            surviving_output_stream.time_base.num != 1 || surviving_output_stream.time_base.den != 90000 ||
            surviving_output_stream.codecpar->extradata_size != sizeof(expected_codec_extra_data) ||
            std::memcmp(surviving_output_stream.codecpar->extradata, expected_codec_extra_data,
                        sizeof(expected_codec_extra_data)) != 0) {
            throw std::runtime_error("A stream de saida deve sobreviver a destruicao dos parametros de origem.");
        }

        AvCodecParametersPtr source_without_extra_data = allocateAvCodecParameters();
        source_without_extra_data->codec_type = AVMEDIA_TYPE_VIDEO;
        source_without_extra_data->codec_id = AV_CODEC_ID_H264;
        source_without_extra_data->width = 1280;
        source_without_extra_data->height = 720;
        VideoStreamParameters parameters_without_extra_data{
            *source_without_extra_data, AVRational{1, 90000}
        };
        AvOutputFormatContextPtr output_without_extra_data = allocateMp4OutputContext();
        AVStream& stream_without_extra_data =
            createMp4VideoStream(*output_without_extra_data, parameters_without_extra_data);
        if (stream_without_extra_data.codecpar->extradata != nullptr ||
            stream_without_extra_data.codecpar->extradata_size != 0) {
            throw std::runtime_error("A configuracao sem extradata deve ser aceita nesta etapa.");
        }

        AvOutputFormatContextPtr unconfigured_output_context{avformat_alloc_context()};
        if (!unconfigured_output_context) {
            throw std::bad_alloc{};
        }
        expect_rejection_without_stream_changes(*unconfigured_output_context, parameters_without_extra_data);
        AVFormatContext* allocated_other_format_context = nullptr;
        const int other_format_allocation_result = avformat_alloc_output_context2(
            &allocated_other_format_context, nullptr, "matroska", nullptr
        );
        AvOutputFormatContextPtr other_format_context{allocated_other_format_context};
        if (other_format_allocation_result < 0 || !other_format_context) {
            throw std::runtime_error("Nao foi possivel preparar um contexto nao MP4 para o teste.");
        }
        expect_rejection_without_stream_changes(*other_format_context, parameters_without_extra_data);

        AVIOContext attached_io_context_marker{};
        AvOutputFormatContextPtr empty_video_output_context = allocateMp4OutputContext();
        const AVInputFormat* input_format_marker = av_find_input_format("h264");
        if (!input_format_marker) {
            throw std::runtime_error("Nao foi possivel obter um formato de entrada para o teste.");
        }
        empty_video_output_context->iformat = input_format_marker;
        expect_rejection_without_stream_changes(*empty_video_output_context, parameters_without_extra_data);
        empty_video_output_context->iformat = nullptr;
        empty_video_output_context->pb = &attached_io_context_marker;
        expect_rejection_without_stream_changes(*empty_video_output_context, parameters_without_extra_data);
        empty_video_output_context->pb = nullptr;

        VideoStreamParameters missing_codec_parameters{
            *source_without_extra_data, AVRational{1, 90000}
        };
        missing_codec_parameters.codec_parameters.reset();
        expect_rejection_without_stream_changes(*empty_video_output_context, missing_codec_parameters);
        const auto expect_source_rejection = [&] {
            VideoStreamParameters invalid_source_parameters{
                *source_without_extra_data, AVRational{1, 90000}
            };
            expect_rejection_without_stream_changes(*empty_video_output_context, invalid_source_parameters);
        };
        for (const AVMediaType invalid_media_type : {AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_UNKNOWN}) {
            source_without_extra_data->codec_type = invalid_media_type;
            expect_source_rejection();
        }
        source_without_extra_data->codec_type = AVMEDIA_TYPE_VIDEO;
        for (const AVCodecID invalid_codec_id : {AV_CODEC_ID_HEVC, AV_CODEC_ID_NONE}) {
            source_without_extra_data->codec_id = invalid_codec_id;
            expect_source_rejection();
        }
        source_without_extra_data->codec_id = AV_CODEC_ID_H264;
        for (const int invalid_dimension : {0, -1}) {
            source_without_extra_data->width = invalid_dimension;
            expect_source_rejection();
            source_without_extra_data->width = 1280;
            source_without_extra_data->height = invalid_dimension;
            expect_source_rejection();
            source_without_extra_data->height = 720;
        }
        const std::array<AVRational, 4> invalid_time_bases{{{0, 1}, {1, 0}, {-1, 1}, {1, -1}}};
        for (const AVRational invalid_time_base : invalid_time_bases) {
            VideoStreamParameters invalid_timestamp_parameters{
                *source_without_extra_data, AVRational{1, 90000}
            };
            invalid_timestamp_parameters.packet_timestamp_time_base = invalid_time_base;
            expect_rejection_without_stream_changes(*empty_video_output_context, invalid_timestamp_parameters);
        }
        for (const int invalid_extra_data_size : {-1, 3}) {
            auto inconsistent_codec_parameters = cloneAvCodecParameters(*source_without_extra_data);
            inconsistent_codec_parameters->extradata_size = invalid_extra_data_size;
            VideoStreamParameters inconsistent_extra_data_parameters{
                *source_without_extra_data, AVRational{1, 90000}
            };
            inconsistent_extra_data_parameters.codec_parameters = std::move(inconsistent_codec_parameters);
            expect_rejection_without_stream_changes(*empty_video_output_context, inconsistent_extra_data_parameters);
        }

        std::cout << "PASS: stream H.264 pertence ao contexto MP4, com codec_tag zerado e base de tempo inicial.\n";
        std::cout << "PASS: parametros e extradata independentes sobrevivem a origem; ausencia de extradata aceita.\n";
        std::cout << "PASS: entradas invalidas e segunda stream rejeitadas sem alterar as streams existentes.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
