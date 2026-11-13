#include "ffmpeg/AvBitstreamFilter.hpp"
#include "EncodedBytesDiagnostics.hpp"
#include "input/FfmpegEncodedVideoSource.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/error.h>
}

int main(int argument_count, char** argument_values) {
    if (argument_count != 2) {
        std::cerr << "Uso: ffmpeg_encoded_video_packets_tests <arquivo-local-h264>\n";
        return 2;
    }

    try {
        EncodedVideoPackets first_packet_batch;
        EncodedVideoPackets remaining_packet_batch;
        std::weak_ptr<const VideoStreamParameters> video_stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> codec_parameters_lifetime;
        std::array<const unsigned char*, 3> original_packet_data_addresses{};
        std::array<std::vector<unsigned char>, 3> original_packet_contents;
        AvPacketPtr retained_filtered_packet;
        const unsigned char* retained_filtered_packet_data_address = nullptr;
        std::vector<unsigned char> retained_filtered_packet_contents;

        {
            FfmpegEncodedVideoSource video_source(argument_values[1]);
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
            const PacketReadEnd invalid_call_end = video_source.getPacketReadEnd();
            if (invalid_call_end.reason != PacketReadEndReason::packet_preparation_error ||
                invalid_call_end.ffmpeg_error_code != 0) {
                throw std::runtime_error("A rejeicao de argumentos deve registrar erro de preparo sem codigo FFmpeg.");
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
                const auto expected_decoder_packets = filterH264PacketToAnnexB(
                    *reference_decoder_filter, *reference_packet
                );
                if (expected_decoder_packets.size() != 1 || !expected_decoder_packets.front() ||
                    expected_decoder_packets.front()->data == nullptr || expected_decoder_packets.front()->size <= 0) {
                    throw std::runtime_error("O filtro de referencia deve produzir uma saida valida por entrada.");
                }
                unsigned char* packet_data = nullptr;
                std::size_t encoded_packet_byte_count = 0;
                if (!video_source.getNextPacket(&packet_data, &encoded_packet_byte_count) ||
                    packet_data == nullptr || encoded_packet_byte_count == 0) {
                    throw std::runtime_error("Nao foi possivel obter os tres pacotes locais de teste.");
                }
                const auto& expected_decoder_packet = expected_decoder_packets.front();
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

            const auto empty_packet_batch = video_source.dequeueEncodedPacketsWithStreamParameters(0);
            if (!empty_packet_batch.encoded_packets.empty() ||
                empty_packet_batch.video_stream_parameters.get() != expected_video_stream_parameters.get()) {
                throw std::runtime_error("Retirar zero pacotes deve retornar uma colecao vazia com configuracao.");
            }

            bool rejected_excessive_packet_count = false;
            try {
                video_source.dequeueEncodedPacketsWithStreamParameters(4);
            } catch (const std::runtime_error&) {
                rejected_excessive_packet_count = true;
            }
            if (!rejected_excessive_packet_count) {
                throw std::runtime_error("A retirada deve rejeitar uma quantidade maior que a fila.");
            }

            first_packet_batch = video_source.dequeueEncodedPacketsWithStreamParameters(2);
            remaining_packet_batch = video_source.dequeueEncodedPacketsWithStreamParameters(1);
            if (first_packet_batch.encoded_packets.size() != 2 ||
                remaining_packet_batch.encoded_packets.size() != 1 ||
                first_packet_batch.video_stream_parameters.get() != expected_video_stream_parameters.get() ||
                remaining_packet_batch.video_stream_parameters.get() != expected_video_stream_parameters.get()) {
                throw std::runtime_error("As retiradas devem preservar as quantidades e compartilhar a configuracao.");
            }

            for (std::size_t packet_index = 0; packet_index < reference_packets.size(); ++packet_index) {
                const auto& queued_packet = packet_index < 2
                    ? first_packet_batch.encoded_packets[packet_index]
                    : remaining_packet_batch.encoded_packets[0];
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

            for (std::size_t packet_index = 0; packet_index < first_packet_batch.encoded_packets.size(); ++packet_index) {
                const auto& original_packet = first_packet_batch.encoded_packets[packet_index];
                const auto original_pts = original_packet->pts;
                const auto original_dts = original_packet->dts;
                const auto original_duration = original_packet->duration;
                const auto original_flags = original_packet->flags;
                const auto original_stream_index = original_packet->stream_index;
                const auto original_position = original_packet->pos;
                const auto original_time_base = original_packet->time_base;
                auto filtered_packets = filterH264PacketToAnnexB(*h264_annex_b_filter, *original_packet);
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
                if (filtered_packets.empty()) {
                    throw std::runtime_error("Cada um dos dois pacotes deste MP4 deve produzir uma saida filtrada.");
                }
                for (const auto& filtered_packet : filtered_packets) {
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
                }
                if (packet_index == 0) {
                    printEncodedBytesPrefix("filtered_decoder_packet", filtered_packets.front()->data,
                        static_cast<std::size_t>(filtered_packets.front()->size));
                    retained_filtered_packet_data_address = filtered_packets.front()->data;
                    retained_filtered_packet_contents.assign(filtered_packets.front()->data,
                        filtered_packets.front()->data + filtered_packets.front()->size);
                    retained_filtered_packet = std::move(filtered_packets.front());
                }
            }

            auto undrained_filter = createH264AnnexBFilter(*codec_parameters, packet_timestamp_time_base);
            auto pending_filter_input = cloneAvPacket(*first_packet_batch.encoded_packets.front());
            if (av_bsf_send_packet(undrained_filter.get(), pending_filter_input.get()) < 0) {
                throw std::runtime_error("Nao foi possivel preparar o caso de filtro com dados pendentes.");
            }
            bool rejected_undrained_filter = false;
            try {
                static_cast<void>(filterH264PacketToAnnexB(
                    *undrained_filter, *first_packet_batch.encoded_packets.front()
                ));
            } catch (const std::runtime_error&) {
                rejected_undrained_filter = true;
            }
            if (!rejected_undrained_filter) {
                throw std::runtime_error("O envio com filtro nao drenado deve informar erro.");
            }

            bool rejected_empty_queue = false;
            try {
                video_source.dequeueEncodedPacketsWithStreamParameters(1);
            } catch (const std::runtime_error&) {
                rejected_empty_queue = true;
            }
            if (!rejected_empty_queue) {
                throw std::runtime_error("A fila deve estar vazia apos retirar os tres pacotes.");
            }
        }

        if (video_stream_parameters_lifetime.expired() || codec_parameters_lifetime.expired() ||
            first_packet_batch.video_stream_parameters->codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            first_packet_batch.video_stream_parameters->codec_parameters->width <= 0 ||
            first_packet_batch.video_stream_parameters->codec_parameters->extradata == nullptr ||
            first_packet_batch.video_stream_parameters->codec_parameters->extradata_size <= 0) {
            throw std::runtime_error("As colecoes devem manter a configuracao valida apos destruir a fonte.");
        }

        if (!retained_filtered_packet ||
            retained_filtered_packet->data != retained_filtered_packet_data_address ||
            retained_filtered_packet->size != static_cast<int>(retained_filtered_packet_contents.size()) ||
            std::memcmp(retained_filtered_packet->data, retained_filtered_packet_contents.data(),
                retained_filtered_packet_contents.size()) != 0) {
            throw std::runtime_error("A saida filtrada deve continuar valida apos destruir o filtro e a fonte.");
        }

        for (std::size_t packet_index = 0; packet_index < original_packet_contents.size(); ++packet_index) {
            const auto& retained_packet = packet_index < 2
                ? first_packet_batch.encoded_packets[packet_index]
                : remaining_packet_batch.encoded_packets[0];
            const auto& expected_packet_contents = original_packet_contents[packet_index];
            const auto& packet_timestamp_time_base =
                first_packet_batch.video_stream_parameters->packet_timestamp_time_base;
            if (!retained_packet ||
                retained_packet->data != original_packet_data_addresses[packet_index] ||
                static_cast<std::size_t>(retained_packet->size) != expected_packet_contents.size() ||
                retained_packet->time_base.num != packet_timestamp_time_base.num ||
                retained_packet->time_base.den != packet_timestamp_time_base.den ||
                std::memcmp(retained_packet->data, expected_packet_contents.data(), expected_packet_contents.size()) != 0) {
                throw std::runtime_error("Os pacotes devem preservar ordem, endereco, conteudo e base de tempo.");
            }
        }

        first_packet_batch = EncodedVideoPackets{};
        if (video_stream_parameters_lifetime.expired() || codec_parameters_lifetime.expired()) {
            throw std::runtime_error("A segunda colecao deve manter a configuracao viva.");
        }

        remaining_packet_batch = EncodedVideoPackets{};
        if (!video_stream_parameters_lifetime.expired() || !codec_parameters_lifetime.expired()) {
            throw std::runtime_error("A ultima colecao deve liberar a configuracao e seus parametros.");
        }

        std::cout << "PASS: filtro H.264 inicializado, parametros independentes e entradas invalidas rejeitadas.\n";
        std::cout << "PASS: dois pacotes filtrados em Annex B, originais intactos e saida com vida util segura.\n";
        std::cout << "PASS: fonte entrega tres pacotes Annex B, preserva originais e extradata e mantem vida util segura.\n";

        {
            FfmpegEncodedVideoSource video_source(argument_values[1]);
            if (video_source.getPacketReadEnd().reason != PacketReadEndReason::not_ended ||
                video_source.getPacketReadEnd().ffmpeg_error_code != 0) {
                throw std::runtime_error("A fonte deve iniciar sem motivo de encerramento.");
            }
            unsigned char* packet_data = nullptr;
            std::size_t packet_size = 0;
            while (video_source.getNextPacket(&packet_data, &packet_size)) {
            }
            const PacketReadEnd packet_read_end = video_source.getPacketReadEnd();
            if (packet_read_end.reason != PacketReadEndReason::end_of_stream ||
                packet_read_end.ffmpeg_error_code != AVERROR_EOF) {
                throw std::runtime_error("E07-T07: a fonte deve registrar fim de arquivo e AVERROR_EOF.");
            }
            if (video_source.getNextPacket(nullptr, &packet_size) ||
                video_source.getPacketReadEnd().reason != PacketReadEndReason::end_of_stream ||
                video_source.getPacketReadEnd().ffmpeg_error_code != AVERROR_EOF) {
                throw std::runtime_error("A primeira causa deve prevalecer sobre falhas posteriores.");
            }
        }
        std::cout << "PASS: E07-T07 fonte registra AVERROR_EOF e preserva a primeira causa.\n";

        {
            std::atomic<bool> keep_running{true};
            FfmpegEncodedVideoSource video_source(argument_values[1], "tcp", &keep_running);
            keep_running.store(false);
            unsigned char* packet_data = nullptr;
            std::size_t packet_size = 0;
            if (video_source.getNextPacket(&packet_data, &packet_size)) {
                throw std::runtime_error("E07-T08: nao deve entregar pacote apos interrupcao.");
            }
            const PacketReadEnd packet_read_end = video_source.getPacketReadEnd();
            if (packet_read_end.reason != PacketReadEndReason::interrupted ||
                packet_read_end.ffmpeg_error_code != AVERROR_EXIT) {
                throw std::runtime_error("E07-T08: motivo ou codigo inesperado ao interromper a leitura.");
            }
        }
        std::cout << "PASS: E07-T08 leitura interrompida retorna falso e registra AVERROR_EXIT.\n";

        {
            std::atomic<bool> keep_running{true};
            FfmpegEncodedVideoSource video_source(argument_values[1], "tcp", &keep_running);
            unsigned char* packet_data = nullptr;
            std::size_t packet_size = 0;
            for (std::size_t packet_index = 0; packet_index < 3; ++packet_index) {
                if (!video_source.getNextPacket(&packet_data, &packet_size)) {
                    throw std::runtime_error("A fonte deve entregar os tres pacotes anteriores a interrupcao.");
                }
            }
            const std::size_t queued_packet_count = video_source.getEncodedPacketQueue().getQueuedPacketCount();
            keep_running.store(false);
            if (video_source.getNextPacket(&packet_data, &packet_size) || packet_data != nullptr || packet_size != 0) {
                throw std::runtime_error("A chamada interrompida nao deve entregar dados ao NVDEC.");
            }
            const PacketReadEnd packet_read_end = video_source.getPacketReadEnd();
            if (packet_read_end.reason != PacketReadEndReason::interrupted ||
                packet_read_end.ffmpeg_error_code != AVERROR_EXIT || queued_packet_count != 3 ||
                video_source.getEncodedPacketQueue().getQueuedPacketCount() != queued_packet_count) {
                throw std::runtime_error("A chamada interrompida deve registrar AVERROR_EXIT sem aumentar a fila.");
            }
        }
        std::cout << "PASS: E07-T08 adicional interrupcao apos tres pacotes preserva o tamanho da fila.\n";

        {
            FfmpegEncodedVideoSource video_source(argument_values[1], "tcp", nullptr, 10);
            unsigned char* packet_data = nullptr;
            std::size_t packet_size = 0;
            for (std::size_t packet_index = 0; packet_index < 10; ++packet_index) {
                if (!video_source.getNextPacket(&packet_data, &packet_size)) {
                    throw std::runtime_error("E09-T04: os dez primeiros pacotes devem ser entregues.");
                }
            }
            if (video_source.getNextPacket(&packet_data, &packet_size) || packet_data != nullptr || packet_size != 0) {
                throw std::runtime_error("E09-T04: a decima primeira chamada deve parar a leitura sem entregar dados.");
            }
            const PacketReadEnd packet_read_end = video_source.getPacketReadEnd();
            if (packet_read_end.reason != PacketReadEndReason::decoder_stalled || packet_read_end.ffmpeg_error_code != 0 ||
                video_source.getMaximumUncountedPacketCount() != 10 ||
                video_source.getEncodedPacketQueue().getQueuedPacketCount() != 10 ||
                video_source.getEncodedPacketQueue().getUncountedPacketCount() != 10) {
                throw std::runtime_error("E09-T04: deve registrar decoder_stalled e preservar os dez pacotes na fila.");
            }
        }
        std::cout << "PASS: E09-T04 limite 10 aceita dez pacotes e rejeita o seguinte sem alterar a fila.\n";

        {
            FfmpegEncodedVideoSource video_source(argument_values[1], "tcp", nullptr, 10);
            unsigned char* packet_data = nullptr;
            std::size_t packet_size = 0;
            for (std::size_t packet_index = 0; packet_index < 100; ++packet_index) {
                if (!video_source.getNextPacket(&packet_data, &packet_size)) {
                    throw std::runtime_error("E09-T05: avancar a fronteira deve impedir disparo do teto.");
                }
                video_source.getEncodedPacketQueue().advanceCountedPackets(1);
            }
            if (video_source.getPacketReadEnd().reason != PacketReadEndReason::not_ended ||
                video_source.getEncodedPacketQueue().getUncountedPacketCount() != 0 ||
                video_source.getEncodedPacketQueue().getQueuedPacketCount() != 100) {
                throw std::runtime_error("E09-T05: o teto deve medir somente pacotes nao contabilizados.");
            }
        }
        std::cout << "PASS: E09-T05 cem leituras com avanco da fronteira nao disparam o teto.\n";

        {
            FfmpegEncodedVideoSource video_source(argument_values[1]);
            const std::size_t expected_limit = static_cast<std::size_t>(std::ceil(video_source.format().fps * 40.0)) + 32;
            if (video_source.getMaximumUncountedPacketCount() != expected_limit) {
                throw std::runtime_error("E09-T06: limite derivado deve ser ceil(fps * 40) + 32.");
            }
            std::cout << "PASS: E09-T06 limite derivado ceil(fps * 40) + 32 = " << expected_limit << ".\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
