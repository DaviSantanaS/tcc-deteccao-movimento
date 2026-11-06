#include "ffmpeg/AvBitstreamFilter.hpp"
#include "EncodedBytesDiagnostics.hpp"
#include "input/FfmpegEncodedVideoSource.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

int main(int argument_count, char** argument_values) {
    if (argument_count != 2) {
        std::cerr << "Uso: ffmpeg_encoded_video_packets_tests <arquivo-local-h264>\n";
        return 2;
    }

    try {
        const auto encoded_video_buffer = std::make_shared<EncodedVideoBuffer>();
        std::array<AvPacketPtr, 3> buffered_packets;
        AVRational source_packet_timestamp_time_base{0, 1};
        std::weak_ptr<const VideoStreamParameters> video_stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> codec_parameters_lifetime;
        std::array<const unsigned char*, 3> original_packet_data_addresses{};
        std::array<std::vector<unsigned char>, 3> original_packet_contents;
        AvPacketPtr retained_filtered_packet;
        const unsigned char* retained_filtered_packet_data_address = nullptr;
        std::vector<unsigned char> retained_filtered_packet_contents;

        {
            bool rejected_missing_buffer = false;
            try {
                FfmpegEncodedVideoSource source_without_buffer(argument_values[1], nullptr);
            } catch (const std::invalid_argument&) {
                rejected_missing_buffer = true;
            }
            if (!rejected_missing_buffer) {
                throw std::runtime_error("A fonte deve rejeitar a ausencia de buffer.");
            }

            FfmpegEncodedVideoSource video_source(argument_values[1], encoded_video_buffer);
            const auto expected_video_stream_parameters = video_source.getVideoStreamParameters();
            video_stream_parameters_lifetime = expected_video_stream_parameters;
            codec_parameters_lifetime = expected_video_stream_parameters->codec_parameters;

            const auto& codec_parameters = expected_video_stream_parameters->codec_parameters;
            printEncodedBytesPrefix(
                "custom_extradata",
                codec_parameters->extradata,
                codec_parameters->extradata_size > 0
                    ? static_cast<std::size_t>(codec_parameters->extradata_size) : 0
            );

            if (codec_parameters->extradata == nullptr || codec_parameters->extradata_size <= 0) {
                throw std::runtime_error("O MP4 de teste deve fornecer extradata H.264.");
            }
            const std::vector<unsigned char> original_codec_extra_data(
                codec_parameters->extradata,
                codec_parameters->extradata + codec_parameters->extradata_size
            );
            const auto packet_timestamp_time_base = expected_video_stream_parameters->packet_timestamp_time_base;
            source_packet_timestamp_time_base = packet_timestamp_time_base;
            auto h264_annex_b_filter = createH264AnnexBFilter(*codec_parameters, packet_timestamp_time_base);
            if (!h264_annex_b_filter ||
                h264_annex_b_filter->par_in == nullptr || h264_annex_b_filter->par_out == nullptr) {
                throw std::runtime_error("O filtro deve possuir parametros de entrada e saida.");
            }
            const auto* filter_input_parameters = h264_annex_b_filter->par_in;
            if (filter_input_parameters == codec_parameters.get() ||
                filter_input_parameters->codec_id != AV_CODEC_ID_H264 ||
                filter_input_parameters->width != codec_parameters->width ||
                filter_input_parameters->height != codec_parameters->height ||
                filter_input_parameters->extradata == nullptr ||
                filter_input_parameters->extradata == codec_parameters->extradata ||
                filter_input_parameters->extradata_size != codec_parameters->extradata_size ||
                std::memcmp(filter_input_parameters->extradata, original_codec_extra_data.data(),
                    original_codec_extra_data.size()) != 0 ||
                std::memcmp(codec_parameters->extradata, original_codec_extra_data.data(),
                    original_codec_extra_data.size()) != 0) {
                throw std::runtime_error("O filtro deve copiar os parametros sem alterar ou compartilhar a extradata original.");
            }
            if (h264_annex_b_filter->time_base_in.num != packet_timestamp_time_base.num ||
                h264_annex_b_filter->time_base_in.den != packet_timestamp_time_base.den ||
                h264_annex_b_filter->time_base_out.num != packet_timestamp_time_base.num ||
                h264_annex_b_filter->time_base_out.den != packet_timestamp_time_base.den) {
                throw std::runtime_error("O filtro deve preservar a base de tempo de entrada e saida.");
            }
            const auto* filter_output_parameters = h264_annex_b_filter->par_out;
            const std::array<unsigned char, 4> annex_b_start_code{0, 0, 0, 1};
            if (filter_output_parameters->codec_id != AV_CODEC_ID_H264 ||
                filter_output_parameters->extradata == nullptr ||
                filter_output_parameters->extradata_size < static_cast<int>(annex_b_start_code.size()) ||
                std::memcmp(filter_output_parameters->extradata, annex_b_start_code.data(),
                    annex_b_start_code.size()) != 0) {
                throw std::runtime_error("A inicializacao deve preparar a extradata de saida em Annex B.");
            }
            printEncodedBytesPrefix(
                "filter_output_extradata", filter_output_parameters->extradata,
                static_cast<std::size_t>(filter_output_parameters->extradata_size)
            );

            AVCodecParameters unsupported_codec_parameters{};
            unsupported_codec_parameters.codec_id = AV_CODEC_ID_HEVC;
            bool rejected_unsupported_codec = false;
            try {
                static_cast<void>(createH264AnnexBFilter(unsupported_codec_parameters, packet_timestamp_time_base));
            } catch (const std::invalid_argument&) {
                rejected_unsupported_codec = true;
            }
            if (!rejected_unsupported_codec) {
                throw std::runtime_error("O filtro deve rejeitar um codec diferente de H.264.");
            }

            const std::array<AVRational, 4> invalid_packet_timestamp_time_bases{{
                {0, 1}, {1, 0}, {-1, 1}, {1, -1}
            }};
            for (const auto invalid_time_base : invalid_packet_timestamp_time_bases) {
                bool rejected_invalid_time_base = false;
                try {
                    static_cast<void>(createH264AnnexBFilter(*codec_parameters, invalid_time_base));
                } catch (const std::invalid_argument&) {
                    rejected_invalid_time_base = true;
                }
                if (!rejected_invalid_time_base) {
                    throw std::runtime_error("O filtro deve rejeitar uma base de tempo nao positiva.");
                }
            }

            auto truncated_codec_parameters = cloneAvCodecParameters(*codec_parameters);
            truncated_codec_parameters->extradata_size = 6;
            bool rejected_truncated_extra_data = false;
            try {
                static_cast<void>(createH264AnnexBFilter(*truncated_codec_parameters, packet_timestamp_time_base));
            } catch (const std::runtime_error&) {
                rejected_truncated_extra_data = true;
            }
            if (!rejected_truncated_extra_data) {
                throw std::runtime_error("A falha de inicializacao com extradata truncada deve ser informada.");
            }

            AVFormatContext* reference_input = nullptr;
            const int reference_input_initialization_status = avformat_open_input(&reference_input, argument_values[1], nullptr, nullptr);
            const auto reference_input_deleter = [](AVFormatContext* input_format_context) { avformat_close_input(&input_format_context); };
            const std::unique_ptr<AVFormatContext, decltype(reference_input_deleter)> reference_context(
                reference_input, reference_input_deleter
            );
            if (reference_input_initialization_status < 0 || avformat_find_stream_info(reference_context.get(), nullptr) < 0) {
                throw std::runtime_error("Nao foi possivel abrir a entrada independente de referencia.");
            }
            const int reference_video_stream_index = av_find_best_stream(
                reference_context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
            );
            if (reference_video_stream_index < 0) {
                throw std::runtime_error("A entrada independente deve fornecer uma stream de video.");
            }
            const auto* reference_video_stream = reference_context->streams[reference_video_stream_index];
            auto reference_decoder_filter = createH264AnnexBFilter(
                *reference_video_stream->codecpar, reference_video_stream->time_base
            );
            std::array<AvPacketPtr, 3> reference_packets;
            unsigned char* invalid_call_data = nullptr;
            std::size_t invalid_call_packet_byte_count = 0;
            if (video_source.getNextPacket(nullptr, &invalid_call_packet_byte_count) ||
                video_source.getNextPacket(&invalid_call_data, nullptr)) {
                throw std::runtime_error("Argumentos nulos devem ser rejeitados sem consumir pacotes.");
            }

            for (std::size_t packet_index = 0; packet_index < original_packet_contents.size(); ++packet_index) {
                auto& reference_packet = reference_packets[packet_index];
                reference_packet = allocateAvPacket();
                while (true) {
                    if (av_read_frame(reference_context.get(), reference_packet.get()) < 0) {
                        throw std::runtime_error("Nao foi possivel ler os tres pacotes independentes de referencia.");
                    }
                    if (reference_packet->stream_index == reference_video_stream_index &&
                        reference_packet->data != nullptr && reference_packet->size > 0) {
                        break;
                    }
                    av_packet_unref(reference_packet.get());
                }
                reference_packet->time_base = reference_video_stream->time_base;
                original_packet_contents[packet_index].assign(
                    reference_packet->data, reference_packet->data + reference_packet->size
                );
                const auto expected_decoder_packet = filterH264PacketToAnnexB(
                    *reference_decoder_filter, *reference_packet
                );
                if (!expected_decoder_packet || expected_decoder_packet->data == nullptr ||
                    expected_decoder_packet->size <= 0) {
                    throw std::runtime_error("O filtro de referencia deve produzir uma saida valida por entrada.");
                }
                unsigned char* packet_data = nullptr;
                std::size_t encoded_packet_byte_count = 0;
                if (!video_source.getNextPacket(&packet_data, &encoded_packet_byte_count) ||
                    packet_data == nullptr || encoded_packet_byte_count == 0) {
                    throw std::runtime_error("Nao foi possivel obter os tres pacotes locais de teste.");
                }
                const bool annex_b_start_code_present = encoded_packet_byte_count >= 3 && packet_data[0] == 0 &&
                    packet_data[1] == 0 && (packet_data[2] == 1 || (encoded_packet_byte_count >= 4 &&
                        packet_data[2] == 0 && packet_data[3] == 1));
                if (!annex_b_start_code_present || encoded_packet_byte_count != static_cast<std::size_t>(expected_decoder_packet->size) ||
                    std::memcmp(packet_data, expected_decoder_packet->data, encoded_packet_byte_count) != 0 ||
                    video_source.lastPacketContainsKeyFrame() != ((reference_packet->flags & AV_PKT_FLAG_KEY) != 0)) {
                    throw std::runtime_error("O decoder deve receber a saida Annex B correta, com indicacao de keyframe preservada.");
                }
                if (packet_index == 0) {
                    printEncodedBytesPrefix("custom_decoder_packet", packet_data, encoded_packet_byte_count);
                    printEncodedBytesPrefix("reference_original_packet", reference_packet->data,
                        static_cast<std::size_t>(reference_packet->size));
                }
            }
            cv::Mat public_codec_extra_data;
            video_source.getExtraData(public_codec_extra_data);
            if (public_codec_extra_data.empty() || public_codec_extra_data.total() != original_codec_extra_data.size() ||
                std::memcmp(public_codec_extra_data.data, original_codec_extra_data.data(),
                    original_codec_extra_data.size()) != 0 ||
                std::memcmp(codec_parameters->extradata, original_codec_extra_data.data(),
                    original_codec_extra_data.size()) != 0) {
                throw std::runtime_error("A entrega Annex B nao deve alterar a extradata publica ou preservada.");
            }

            if (encoded_video_buffer->getPacketCount() != reference_packets.size()) {
                throw std::runtime_error("Cada getNextPacket deve guardar exatamente um pacote no buffer.");
            }
            for (auto& buffered_packet : buffered_packets) {
                buffered_packet = encoded_video_buffer->tryPopPacket();
            }

            for (std::size_t packet_index = 0; packet_index < reference_packets.size(); ++packet_index) {
                const auto& queued_packet = buffered_packets[packet_index];
                const auto& reference_packet = reference_packets[packet_index];
                if (!queued_packet || queued_packet->data == nullptr || queued_packet->size != reference_packet->size ||
                    queued_packet->pts != reference_packet->pts || queued_packet->dts != reference_packet->dts ||
                    queued_packet->duration != reference_packet->duration || queued_packet->flags != reference_packet->flags ||
                    queued_packet->stream_index != reference_packet->stream_index || queued_packet->pos != reference_packet->pos ||
                    queued_packet->time_base.num != reference_packet->time_base.num ||
                    queued_packet->time_base.den != reference_packet->time_base.den ||
                    std::memcmp(queued_packet->data, reference_packet->data, static_cast<std::size_t>(reference_packet->size)) != 0) {
                    throw std::runtime_error("A fila deve preservar os bytes e metadados originais obtidos por demux independente.");
                }
                original_packet_data_addresses[packet_index] = queued_packet->data;
            }

            auto empty_encoded_packet = allocateAvPacket();
            bool rejected_empty_packet = false;
            try {
                static_cast<void>(filterH264PacketToAnnexB(*h264_annex_b_filter, *empty_encoded_packet));
            } catch (const std::invalid_argument&) {
                rejected_empty_packet = true;
            }
            if (!rejected_empty_packet) {
                throw std::runtime_error("Um pacote vazio deve ser rejeitado sem encerrar o filtro.");
            }

            for (std::size_t packet_index = 0; packet_index < 2; ++packet_index) {
                const auto& original_packet = buffered_packets[packet_index];
                const auto original_pts = original_packet->pts;
                const auto original_dts = original_packet->dts;
                const auto original_duration = original_packet->duration;
                const auto original_flags = original_packet->flags;
                const auto original_stream_index = original_packet->stream_index;
                const auto original_position = original_packet->pos;
                const auto original_time_base = original_packet->time_base;
                auto filtered_packet = filterH264PacketToAnnexB(*h264_annex_b_filter, *original_packet);
                const auto& expected_original_contents = original_packet_contents[packet_index];
                if (original_packet->data != original_packet_data_addresses[packet_index] ||
                    original_packet->size != static_cast<int>(expected_original_contents.size()) ||
                    original_packet->pts != original_pts || original_packet->dts != original_dts ||
                    original_packet->duration != original_duration || original_packet->flags != original_flags ||
                    original_packet->stream_index != original_stream_index || original_packet->pos != original_position ||
                    original_packet->time_base.num != original_time_base.num ||
                    original_packet->time_base.den != original_time_base.den ||
                    std::memcmp(original_packet->data, expected_original_contents.data(), expected_original_contents.size()) != 0) {
                    throw std::runtime_error("A filtragem nao deve consumir nem alterar o pacote original.");
                }
                if (!filtered_packet || filtered_packet->data == nullptr || filtered_packet->size <= 0) {
                    throw std::runtime_error("A saida filtrada deve possuir dados validos.");
                }
                const bool three_byte_start_code_present = filtered_packet->size >= 3 &&
                    filtered_packet->data[0] == 0 && filtered_packet->data[1] == 0 && filtered_packet->data[2] == 1;
                const bool four_byte_start_code_present = filtered_packet->size >= static_cast<int>(annex_b_start_code.size()) &&
                    std::memcmp(filtered_packet->data, annex_b_start_code.data(), annex_b_start_code.size()) == 0;
                if ((!three_byte_start_code_present && !four_byte_start_code_present) ||
                    filtered_packet->data == original_packet->data ||
                    filtered_packet->pts != original_pts || filtered_packet->dts != original_dts ||
                    filtered_packet->duration != original_duration || filtered_packet->flags != original_flags ||
                    filtered_packet->stream_index != original_stream_index ||
                    filtered_packet->time_base.num != h264_annex_b_filter->time_base_out.num ||
                    filtered_packet->time_base.den != h264_annex_b_filter->time_base_out.den) {
                    throw std::runtime_error("A saida deve estar em Annex B, com dados independentes e metadados preservados.");
                }
                if (packet_index == 0) {
                    printEncodedBytesPrefix("filtered_decoder_packet", filtered_packet->data,
                        static_cast<std::size_t>(filtered_packet->size));
                    retained_filtered_packet_data_address = filtered_packet->data;
                    retained_filtered_packet_contents.assign(filtered_packet->data,
                        filtered_packet->data + filtered_packet->size);
                    retained_filtered_packet = std::move(filtered_packet);
                }
            }

            auto undrained_filter = createH264AnnexBFilter(*codec_parameters, packet_timestamp_time_base);
            auto pending_filter_input = cloneAvPacket(*buffered_packets.front());
            if (av_bsf_send_packet(undrained_filter.get(), pending_filter_input.get()) < 0) {
                throw std::runtime_error("Nao foi possivel preparar o caso de filtro com dados pendentes.");
            }
            bool rejected_undrained_filter = false;
            try {
                static_cast<void>(filterH264PacketToAnnexB(
                    *undrained_filter, *buffered_packets.front()
                ));
            } catch (const std::runtime_error&) {
                rejected_undrained_filter = true;
            }
            if (!rejected_undrained_filter) {
                throw std::runtime_error("O envio com filtro nao drenado deve informar erro.");
            }

            if (encoded_video_buffer->tryPopPacket() != nullptr || encoded_video_buffer->getPacketCount() != 0) {
                throw std::runtime_error("O buffer deve estar vazio apos retirar os tres pacotes.");
            }
        }

        if (!video_stream_parameters_lifetime.expired() || !codec_parameters_lifetime.expired()) {
            throw std::runtime_error("Destruir a fonte deve liberar a configuracao; os pacotes nao dependem dela.");
        }

        if (!retained_filtered_packet ||
            retained_filtered_packet->data != retained_filtered_packet_data_address ||
            retained_filtered_packet->size != static_cast<int>(retained_filtered_packet_contents.size()) ||
            std::memcmp(retained_filtered_packet->data, retained_filtered_packet_contents.data(),
                retained_filtered_packet_contents.size()) != 0) {
            throw std::runtime_error("A saida filtrada deve continuar valida apos destruir o filtro e a fonte.");
        }

        for (std::size_t packet_index = 0; packet_index < original_packet_contents.size(); ++packet_index) {
            const auto& retained_packet = buffered_packets[packet_index];
            const auto& expected_packet_contents = original_packet_contents[packet_index];
            const auto& packet_timestamp_time_base = source_packet_timestamp_time_base;
            if (!retained_packet ||
                retained_packet->data != original_packet_data_addresses[packet_index] ||
                static_cast<std::size_t>(retained_packet->size) != expected_packet_contents.size() ||
                retained_packet->time_base.num != packet_timestamp_time_base.num ||
                retained_packet->time_base.den != packet_timestamp_time_base.den ||
                std::memcmp(retained_packet->data, expected_packet_contents.data(), expected_packet_contents.size()) != 0) {
                throw std::runtime_error("Os pacotes devem preservar ordem, endereco, conteudo e base de tempo.");
            }
        }

        if (encoded_video_buffer.use_count() != 1) {
            throw std::runtime_error("Destruir a fonte deve liberar sua referencia ao buffer.");
        }

        std::cout << "PASS: filtro H.264 inicializado, parametros independentes e entradas invalidas rejeitadas.\n";
        std::cout << "PASS: dois pacotes filtrados em Annex B, originais intactos e saida com vida util segura.\n";
        std::cout << "PASS: fonte entrega tres pacotes Annex B ao NVDEC e guarda os originais no buffer, com vida util segura.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
