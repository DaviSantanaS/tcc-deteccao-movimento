#include "EncodedVideoBuffer.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

int main() {
    try {
        EncodedVideoBuffer default_buffer;
        EncodedVideoBuffer single_read_limit_buffer(1);
        EncodedVideoBuffer three_read_limit_buffer(3);
        EncodedVideoBuffer maximum_read_limit_buffer(
            std::numeric_limits<std::size_t>::max()
        );

        bool rejected_zero_read_limit = false;
        try {
            EncodedVideoBuffer invalid_buffer(0);
        } catch (const std::invalid_argument&) {
            rejected_zero_read_limit = true;
        }

        if (!rejected_zero_read_limit) {
            throw std::runtime_error("O construtor deve rejeitar limite zero de leituras.");
        }

        std::cout << "PASS: construtor aceita limites positivos de leituras e rejeita zero.\n";

        for (std::size_t read_index = 0; read_index < 1000; ++read_index) {
            std::vector<AvPacketPtr> empty_read;
            if (default_buffer.bufferEncodedPackets(std::move(empty_read)) != (read_index == 999)) {
                throw std::runtime_error("O limite padrao deve continuar sendo de 1000 leituras.");
            }
        }

        {
            EncodedVideoBuffer read_packets_buffer(3);
            std::array<const AVPacket*, 3> retained_packet_addresses{};
            std::array<const unsigned char*, 3> retained_packet_data_addresses{};
            {
                std::array<std::vector<AvPacketPtr>, 3> packets_from_reads;
                for (std::size_t packet_index = 0; packet_index < 3; ++packet_index) {
                    auto encoded_packet = allocateAvPacket();
                    if (av_new_packet(encoded_packet.get(), 1) < 0) {
                        throw std::runtime_error("Nao foi possivel alocar o pacote de teste.");
                    }
                    encoded_packet->data[0] = static_cast<unsigned char>(42 + packet_index);
                    encoded_packet->pts = static_cast<int64_t>(123 + packet_index);
                    encoded_packet->dts = static_cast<int64_t>(120 + packet_index);
                    encoded_packet->duration = static_cast<int64_t>(3 + packet_index);
                    encoded_packet->stream_index = 5;
                    encoded_packet->pos = static_cast<int64_t>(900 + packet_index);
                    encoded_packet->flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
                    encoded_packet->time_base = AVRational{1, 90000};
                    retained_packet_addresses[packet_index] = encoded_packet.get();
                    retained_packet_data_addresses[packet_index] = encoded_packet->data;
                    const std::size_t read_index = packet_index < 2 ? 0 : 2;
                    packets_from_reads[read_index].push_back(std::move(encoded_packet));
                }

                for (std::size_t read_index = 0; read_index < packets_from_reads.size(); ++read_index) {
                    const bool read_limit_reached =
                        read_packets_buffer.bufferEncodedPackets(std::move(packets_from_reads[read_index]));
                    if (read_limit_reached != (read_index == 2)) {
                        throw std::runtime_error("O limite deve contar leituras, inclusive a sem pacotes.");
                    }
                    if (!packets_from_reads[read_index].empty()) {
                        throw std::runtime_error("A insercao deve mover os pacotes e esvaziar o vetor recebido.");
                    }
                }

                std::vector<AvPacketPtr> extra_read_packets;
                auto extra_packet = allocateAvPacket();
                if (av_new_packet(extra_packet.get(), 1) < 0) {
                    throw std::runtime_error("Nao foi possivel alocar o pacote da leitura extra.");
                }
                extra_packet->data[0] = 99;
                const auto* extra_packet_address = extra_packet.get();
                const auto* extra_packet_data_address = extra_packet->data;
                extra_read_packets.push_back(std::move(extra_packet));

                bool rejected_extra_read = false;
                try {
                    read_packets_buffer.bufferEncodedPackets(std::move(extra_read_packets));
                } catch (const std::length_error&) {
                    rejected_extra_read = true;
                }
                if (!rejected_extra_read || extra_read_packets.size() != 1 ||
                    extra_read_packets.front().get() != extra_packet_address ||
                    extra_read_packets.front()->data != extra_packet_data_address ||
                    extra_read_packets.front()->data[0] != 99) {
                    throw std::runtime_error("O buffer cheio deve rejeitar a leitura sem consumir sua entrada.");
                }

                EncodedVideoBuffer single_read_buffer(1);
                std::vector<AvPacketPtr> read_without_packets;
                if (!single_read_buffer.bufferEncodedPackets(std::move(read_without_packets)) ||
                    !read_without_packets.empty()) {
                    throw std::runtime_error("Com limite um, a primeira leitura deve preencher o buffer.");
                }
            }

            for (std::size_t packet_index = 0; packet_index < retained_packet_addresses.size(); ++packet_index) {
                const auto* retained_packet = retained_packet_addresses[packet_index];
                const int expected_flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
                if (retained_packet->data != retained_packet_data_addresses[packet_index] ||
                    retained_packet->size != 1 ||
                    retained_packet->data[0] != static_cast<unsigned char>(42 + packet_index) ||
                    retained_packet->pts != static_cast<int64_t>(123 + packet_index) ||
                    retained_packet->dts != static_cast<int64_t>(120 + packet_index) ||
                    retained_packet->duration != static_cast<int64_t>(3 + packet_index) ||
                    retained_packet->stream_index != 5 ||
                    retained_packet->pos != static_cast<int64_t>(900 + packet_index) ||
                    retained_packet->flags != expected_flags ||
                    retained_packet->time_base.num != 1 || retained_packet->time_base.den != 90000) {
                    throw std::runtime_error("Os pacotes originais e seus metadados devem permanecer intactos.");
                }
            }
        }
        std::cout << "PASS: insercao conta leituras de 2, 0 e 1 pacotes, sinaliza o limite e preserva a entrada rejeitada.\n";
        std::cout << "PASS: limite padrao de 1000 leituras; buffer mantem pacotes e metadados sem copiar seus dados.\n";

        std::array<std::vector<AvPacketPtr>, 2> received_read_packets_by_cycle;
        std::array<std::array<const AVPacket*, 3>, 2> packet_addresses_by_cycle{};
        std::array<std::array<const unsigned char*, 3>, 2> packet_data_addresses_by_cycle{};
        const std::array<std::size_t, 3> packet_counts_by_read{2, 0, 1};
        {
            EncodedVideoBuffer reusable_read_packets_buffer(3);
            if (!reusable_read_packets_buffer.takeBufferedEncodedPackets().empty()) {
                throw std::runtime_error("Retirar de um buffer vazio deve devolver um vetor vazio.");
            }
            std::vector<AvPacketPtr> partial_read_packets;
            if (reusable_read_packets_buffer.bufferEncodedPackets(std::move(partial_read_packets))) {
                throw std::runtime_error("Uma leitura parcial nao deve atingir o limite tres.");
            }
            {
                const auto received_partial_read_packets =
                    reusable_read_packets_buffer.takeBufferedEncodedPackets();
                if (!received_partial_read_packets.empty()) {
                    throw std::runtime_error("Retirar depois de uma leitura sem pacotes deve devolver um vetor vazio.");
                }
            }

            for (std::size_t cycle_index = 0; cycle_index < received_read_packets_by_cycle.size(); ++cycle_index) {
                std::size_t packet_index = 0;
                for (std::size_t read_index = 0; read_index < packet_counts_by_read.size(); ++read_index) {
                    std::vector<AvPacketPtr> packets_from_read;
                    for (std::size_t read_packet_index = 0;
                         read_packet_index < packet_counts_by_read[read_index]; ++read_packet_index) {
                        auto encoded_packet = allocateAvPacket();
                        if (av_new_packet(encoded_packet.get(), 1) < 0) {
                            throw std::runtime_error("Nao foi possivel alocar o pacote para testar a retirada.");
                        }
                        encoded_packet->data[0] = static_cast<unsigned char>(100 + cycle_index * 10 + packet_index);
                        encoded_packet->pts = static_cast<int64_t>(1000 + cycle_index * 10 + packet_index);
                        encoded_packet->dts = static_cast<int64_t>(900 + cycle_index * 10 + packet_index);
                        encoded_packet->duration = static_cast<int64_t>(3 + packet_index);
                        encoded_packet->flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
                        encoded_packet->time_base = AVRational{1, 90000};
                        packet_addresses_by_cycle[cycle_index][packet_index] = encoded_packet.get();
                        packet_data_addresses_by_cycle[cycle_index][packet_index] = encoded_packet->data;
                        packets_from_read.push_back(std::move(encoded_packet));
                        ++packet_index;
                    }
                    const bool read_limit_reached =
                        reusable_read_packets_buffer.bufferEncodedPackets(std::move(packets_from_read));
                    if (read_limit_reached != (read_index == 2) || !packets_from_read.empty()) {
                        throw std::runtime_error("A retirada deve reiniciar a contagem e manter o limite em cada ciclo.");
                    }
                }
                received_read_packets_by_cycle[cycle_index] =
                    reusable_read_packets_buffer.takeBufferedEncodedPackets();
                if (!reusable_read_packets_buffer.takeBufferedEncodedPackets().empty()) {
                    throw std::runtime_error("Uma nova retirada sem insercoes deve continuar vazia.");
                }
            }
        }

        for (std::size_t cycle_index = 0; cycle_index < received_read_packets_by_cycle.size(); ++cycle_index) {
            const auto& received_read_packets = received_read_packets_by_cycle[cycle_index];
            if (received_read_packets.size() != packet_addresses_by_cycle[cycle_index].size()) {
                throw std::runtime_error("Cada ciclo deve devolver somente seus tres pacotes.");
            }
            for (std::size_t packet_index = 0; packet_index < received_read_packets.size(); ++packet_index) {
                const auto& received_packet = received_read_packets[packet_index];
                if (!received_packet ||
                    received_packet.get() != packet_addresses_by_cycle[cycle_index][packet_index] ||
                    received_packet->data != packet_data_addresses_by_cycle[cycle_index][packet_index] ||
                    received_packet->size != 1 ||
                    received_packet->data[0] != static_cast<unsigned char>(100 + cycle_index * 10 + packet_index) ||
                    received_packet->pts != static_cast<int64_t>(1000 + cycle_index * 10 + packet_index) ||
                    received_packet->dts != static_cast<int64_t>(900 + cycle_index * 10 + packet_index) ||
                    received_packet->duration != static_cast<int64_t>(3 + packet_index) ||
                    received_packet->flags != (packet_index == 0 ? AV_PKT_FLAG_KEY : 0) ||
                    received_packet->time_base.num != 1 || received_packet->time_base.den != 90000) {
                    throw std::runtime_error("A retirada deve preservar enderecos, bytes e metadados sem misturar ciclos.");
                }
            }
        }
        received_read_packets_by_cycle[0].clear();
        if (received_read_packets_by_cycle[1].front()->data[0] != 110) {
            throw std::runtime_error("Liberar o primeiro resultado nao deve invalidar os pacotes do segundo.");
        }
        received_read_packets_by_cycle[1].clear();
        std::cout << "PASS: retirada vazia e parcial, dois ciclos independentes e limite preservado apos reiniciar a contagem.\n";
        std::cout << "PASS: vetor unico preserva ordem, enderecos, bytes e metadados; resultados sobrevivem ao buffer.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
