#include "EncodedVideoBuffer.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

int main() {
    try {
        EncodedVideoBuffer default_buffer;
        EncodedVideoBuffer single_frame_buffer(1);
        EncodedVideoBuffer three_frame_buffer(3);
        EncodedVideoBuffer maximum_frame_limit_buffer(
            std::numeric_limits<std::size_t>::max()
        );

        bool rejected_zero_frame_limit = false;
        try {
            EncodedVideoBuffer invalid_buffer(0);
        } catch (const std::invalid_argument&) {
            rejected_zero_frame_limit = true;
        }

        if (!rejected_zero_frame_limit) {
            throw std::runtime_error("O construtor deve rejeitar limite zero de frames decodificados.");
        }

        std::cout << "PASS: construtor aceita limites positivos de frames decodificados e rejeita zero.\n";

        std::weak_ptr<const VideoStreamParameters> stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> codec_parameters_lifetime;
        {
            auto source_codec_parameters = allocateAvCodecParameters();
            source_codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            source_codec_parameters->codec_id = AV_CODEC_ID_H264;
            source_codec_parameters->width = 1920;
            source_codec_parameters->height = 1080;

            auto video_stream_parameters = std::make_shared<const VideoStreamParameters>(
                *source_codec_parameters,
                AVRational{1, 90000}
            );
            stream_parameters_lifetime = video_stream_parameters;
            codec_parameters_lifetime = video_stream_parameters->codec_parameters;
            const auto* stream_parameters_address = video_stream_parameters.get();

            EncodedVideoBuffer read_packets_buffer(3);
            std::array<const AVPacket*, 3> retained_packet_addresses{};
            std::array<const unsigned char*, 3> retained_packet_data_addresses{};
            {
                std::array<EncodedVideoPackets, 3> packets_from_reads;
                for (auto& packets_from_read : packets_from_reads) {
                    packets_from_read.video_stream_parameters = video_stream_parameters;
                }
                for (std::size_t packet_index = 0; packet_index < 3; ++packet_index) {
                    auto encoded_packet = allocateAvPacket();
                    if (av_new_packet(encoded_packet.get(), 1) < 0) {
                        throw std::runtime_error("Nao foi possivel alocar o pacote de teste.");
                    }
                    encoded_packet->data[0] = static_cast<unsigned char>(42 + packet_index);
                    encoded_packet->pts = static_cast<int64_t>(123 + packet_index);
                    encoded_packet->dts = static_cast<int64_t>(120 + packet_index);
                    encoded_packet->flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
                    encoded_packet->time_base = AVRational{1, 90000};
                    retained_packet_addresses[packet_index] = encoded_packet.get();
                    retained_packet_data_addresses[packet_index] = encoded_packet->data;
                    const std::size_t read_index = packet_index < 2 ? 0 : 2;
                    packets_from_reads[read_index].encoded_packets.push_back(std::move(encoded_packet));
                }

                for (std::size_t read_index = 0; read_index < packets_from_reads.size(); ++read_index) {
                    const bool decoded_frame_limit_reached =
                        read_packets_buffer.bufferPacketsFromRead(std::move(packets_from_reads[read_index]));
                    if (decoded_frame_limit_reached != (read_index == 2)) {
                        throw std::runtime_error("O limite deve contar leituras, inclusive a sem pacotes.");
                    }
                    if (!packets_from_reads[read_index].encoded_packets.empty() ||
                        packets_from_reads[read_index].video_stream_parameters) {
                        throw std::runtime_error("A insercao deve mover a colecao inteira para o buffer.");
                    }
                }

                EncodedVideoPackets extra_read_packets{{}, video_stream_parameters};
                auto extra_packet = allocateAvPacket();
                if (av_new_packet(extra_packet.get(), 1) < 0) {
                    throw std::runtime_error("Nao foi possivel alocar o pacote da leitura extra.");
                }
                extra_packet->data[0] = 99;
                const auto* extra_packet_address = extra_packet.get();
                const auto* extra_packet_data_address = extra_packet->data;
                extra_read_packets.encoded_packets.push_back(std::move(extra_packet));

                bool rejected_extra_read = false;
                try {
                    read_packets_buffer.bufferPacketsFromRead(std::move(extra_read_packets));
                } catch (const std::length_error&) {
                    rejected_extra_read = true;
                }
                if (!rejected_extra_read || extra_read_packets.encoded_packets.size() != 1 ||
                    extra_read_packets.encoded_packets.front().get() != extra_packet_address ||
                    extra_read_packets.encoded_packets.front()->data != extra_packet_data_address ||
                    extra_read_packets.encoded_packets.front()->data[0] != 99 ||
                    extra_read_packets.video_stream_parameters.get() != stream_parameters_address) {
                    throw std::runtime_error("O buffer cheio deve rejeitar a leitura sem consumir sua entrada.");
                }

                EncodedVideoBuffer single_read_buffer(1);
                EncodedVideoPackets read_without_packets{{}, video_stream_parameters};
                if (!single_read_buffer.bufferPacketsFromRead(std::move(read_without_packets)) ||
                    read_without_packets.video_stream_parameters) {
                    throw std::runtime_error("Com limite um, a primeira leitura deve preencher o buffer.");
                }
            }

            video_stream_parameters.reset();
            source_codec_parameters.reset();
            if (stream_parameters_lifetime.expired() || codec_parameters_lifetime.expired() ||
                stream_parameters_address->codec_parameters->width != 1920 ||
                stream_parameters_address->packet_timestamp_time_base.num != 1 ||
                stream_parameters_address->packet_timestamp_time_base.den != 90000) {
                throw std::runtime_error("O buffer deve manter a configuracao viva apos destruir as entradas.");
            }
            for (std::size_t packet_index = 0; packet_index < retained_packet_addresses.size(); ++packet_index) {
                const auto* retained_packet = retained_packet_addresses[packet_index];
                const int expected_flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
                if (retained_packet->data != retained_packet_data_addresses[packet_index] ||
                    retained_packet->size != 1 ||
                    retained_packet->data[0] != static_cast<unsigned char>(42 + packet_index) ||
                    retained_packet->pts != static_cast<int64_t>(123 + packet_index) ||
                    retained_packet->dts != static_cast<int64_t>(120 + packet_index) ||
                    retained_packet->flags != expected_flags ||
                    retained_packet->time_base.num != 1 || retained_packet->time_base.den != 90000) {
                    throw std::runtime_error("Os pacotes originais e seus metadados devem permanecer intactos.");
                }
            }
        }
        if (!stream_parameters_lifetime.expired() || !codec_parameters_lifetime.expired()) {
            throw std::runtime_error("Destruir o buffer deve liberar as ultimas referencias da configuracao.");
        }
        std::cout << "PASS: insercao conta leituras de 2, 0 e 1 pacotes, sinaliza o limite e preserva a entrada rejeitada.\n";
        std::cout << "PASS: buffer mantem pacotes e configuracao vivos sem copiar seus dados e libera suas referencias.\n";

        std::array<std::vector<EncodedVideoPackets>, 2> received_read_packets_by_cycle;
        std::array<std::array<const AVPacket*, 3>, 2> packet_addresses_by_cycle{};
        std::array<std::array<const unsigned char*, 3>, 2> packet_data_addresses_by_cycle{};
        std::weak_ptr<const VideoStreamParameters> transferred_stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> transferred_codec_parameters_lifetime;
        const VideoStreamParameters* transferred_stream_parameters_address = nullptr;
        const std::array<std::size_t, 3> packet_counts_by_read{2, 0, 1};
        {
            auto source_codec_parameters = allocateAvCodecParameters();
            source_codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            source_codec_parameters->codec_id = AV_CODEC_ID_H264;
            source_codec_parameters->width = 1280;
            source_codec_parameters->height = 720;
            auto video_stream_parameters = std::make_shared<const VideoStreamParameters>(
                *source_codec_parameters, AVRational{1, 90000}
            );
            transferred_stream_parameters_lifetime = video_stream_parameters;
            transferred_codec_parameters_lifetime = video_stream_parameters->codec_parameters;
            transferred_stream_parameters_address = video_stream_parameters.get();

            EncodedVideoBuffer reusable_read_packets_buffer(3);
            if (!reusable_read_packets_buffer.takeBufferedReadPackets().empty()) {
                throw std::runtime_error("Retirar de um buffer vazio deve devolver um vetor vazio.");
            }
            EncodedVideoPackets partial_read_packets{{}, video_stream_parameters};
            if (reusable_read_packets_buffer.bufferPacketsFromRead(std::move(partial_read_packets))) {
                throw std::runtime_error("Uma leitura parcial nao deve atingir o limite tres.");
            }
            {
                const auto received_partial_read_packets =
                    reusable_read_packets_buffer.takeBufferedReadPackets();
                if (received_partial_read_packets.size() != 1 ||
                    !received_partial_read_packets.front().encoded_packets.empty() ||
                    received_partial_read_packets.front().video_stream_parameters.get() !=
                        transferred_stream_parameters_address) {
                    throw std::runtime_error("A retirada parcial deve preservar a leitura sem pacotes e sua configuracao.");
                }
            }

            for (std::size_t cycle_index = 0; cycle_index < received_read_packets_by_cycle.size(); ++cycle_index) {
                std::size_t packet_index = 0;
                for (std::size_t read_index = 0; read_index < packet_counts_by_read.size(); ++read_index) {
                    EncodedVideoPackets packets_from_read{{}, video_stream_parameters};
                    for (std::size_t read_packet_index = 0;
                         read_packet_index < packet_counts_by_read[read_index]; ++read_packet_index) {
                        auto encoded_packet = allocateAvPacket();
                        if (av_new_packet(encoded_packet.get(), 1) < 0) {
                            throw std::runtime_error("Nao foi possivel alocar o pacote para testar a retirada.");
                        }
                        encoded_packet->data[0] = static_cast<unsigned char>(100 + cycle_index * 10 + packet_index);
                        encoded_packet->pts = static_cast<int64_t>(1000 + cycle_index * 10 + packet_index);
                        encoded_packet->dts = static_cast<int64_t>(900 + cycle_index * 10 + packet_index);
                        encoded_packet->flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
                        encoded_packet->time_base = AVRational{1, 90000};
                        packet_addresses_by_cycle[cycle_index][packet_index] = encoded_packet.get();
                        packet_data_addresses_by_cycle[cycle_index][packet_index] = encoded_packet->data;
                        packets_from_read.encoded_packets.push_back(std::move(encoded_packet));
                        ++packet_index;
                    }
                    const bool decoded_frame_limit_reached =
                        reusable_read_packets_buffer.bufferPacketsFromRead(std::move(packets_from_read));
                    if (decoded_frame_limit_reached != (read_index == 2)) {
                        throw std::runtime_error("A retirada deve reiniciar a contagem e manter o limite em cada ciclo.");
                    }
                }
                received_read_packets_by_cycle[cycle_index] =
                    reusable_read_packets_buffer.takeBufferedReadPackets();
                if (!reusable_read_packets_buffer.takeBufferedReadPackets().empty()) {
                    throw std::runtime_error("Uma nova retirada sem insercoes deve continuar vazia.");
                }
            }
            video_stream_parameters.reset();
            source_codec_parameters.reset();
        }

        if (transferred_stream_parameters_lifetime.expired() ||
            transferred_codec_parameters_lifetime.expired()) {
            throw std::runtime_error("Os resultados devem manter a configuracao viva depois de destruir o buffer.");
        }
        for (std::size_t cycle_index = 0; cycle_index < received_read_packets_by_cycle.size(); ++cycle_index) {
            const auto& received_read_packets = received_read_packets_by_cycle[cycle_index];
            if (received_read_packets.size() != packet_counts_by_read.size()) {
                throw std::runtime_error("Cada ciclo deve devolver somente suas tres colecoes.");
            }
            std::size_t packet_index = 0;
            for (std::size_t read_index = 0; read_index < received_read_packets.size(); ++read_index) {
                const auto& received_collection = received_read_packets[read_index];
                if (received_collection.encoded_packets.size() != packet_counts_by_read[read_index] ||
                    received_collection.video_stream_parameters.get() != transferred_stream_parameters_address ||
                    received_collection.video_stream_parameters->codec_parameters->width != 1280 ||
                    received_collection.video_stream_parameters->codec_parameters->height != 720 ||
                    received_collection.video_stream_parameters->packet_timestamp_time_base.num != 1 ||
                    received_collection.video_stream_parameters->packet_timestamp_time_base.den != 90000) {
                    throw std::runtime_error("A retirada deve preservar os grupos em ordem e compartilhar a configuracao.");
                }
                for (const auto& received_packet : received_collection.encoded_packets) {
                    if (!received_packet ||
                        received_packet.get() != packet_addresses_by_cycle[cycle_index][packet_index] ||
                        received_packet->data != packet_data_addresses_by_cycle[cycle_index][packet_index] ||
                        received_packet->size != 1 ||
                        received_packet->data[0] != static_cast<unsigned char>(100 + cycle_index * 10 + packet_index) ||
                        received_packet->pts != static_cast<int64_t>(1000 + cycle_index * 10 + packet_index) ||
                        received_packet->dts != static_cast<int64_t>(900 + cycle_index * 10 + packet_index) ||
                        received_packet->flags != (packet_index == 0 ? AV_PKT_FLAG_KEY : 0) ||
                        received_packet->time_base.num != 1 || received_packet->time_base.den != 90000) {
                        throw std::runtime_error("A retirada deve preservar enderecos, bytes e metadados sem misturar ciclos.");
                    }
                    ++packet_index;
                }
            }
        }
        received_read_packets_by_cycle[0].clear();
        if (transferred_stream_parameters_lifetime.expired() ||
            transferred_codec_parameters_lifetime.expired()) {
            throw std::runtime_error("Liberar o primeiro resultado nao deve invalidar a configuracao do segundo.");
        }
        received_read_packets_by_cycle[1].clear();
        if (!transferred_stream_parameters_lifetime.expired() ||
            !transferred_codec_parameters_lifetime.expired()) {
            throw std::runtime_error("Liberar o ultimo resultado deve liberar as referencias da configuracao.");
        }
        std::cout << "PASS: retirada vazia e parcial, dois ciclos independentes e limite preservado apos reiniciar a contagem.\n";
        std::cout << "PASS: grupos, pacotes e metadados preservados sem copias; resultados sobrevivem ao buffer e liberam suas referencias.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
