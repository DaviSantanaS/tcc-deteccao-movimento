#include "buffers/EncodedVideoBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

// Cria um pacote com um byte de dados e o indice no pts, para conferir a ordem.
AvPacketPtr createIndexedPacket(std::int64_t packet_index) {
    AvPacketPtr encoded_packet = allocateAvPacket();
    if (av_new_packet(encoded_packet.get(), 1) < 0) {
        throw std::runtime_error("Nao foi possivel alocar os dados do pacote de teste.");
    }
    encoded_packet->data[0] = static_cast<unsigned char>(packet_index & 0xFF);
    encoded_packet->pts = packet_index;
    encoded_packet->dts = packet_index;
    return encoded_packet;
}

}

int main() {
    try {
        {
            EncodedVideoBuffer encoded_video_buffer;
            if (encoded_video_buffer.getPacketCount() != 0 || encoded_video_buffer.tryPopPacket() != nullptr) {
                throw std::runtime_error("Um buffer novo deve estar vazio e devolver nullptr.");
            }
            std::cout << "PASS: buffer vazio informa zero pacotes e devolve nullptr.\n";
        }

        {
            EncodedVideoBuffer encoded_video_buffer;
            bool rejected_null_packet = false;
            try {
                encoded_video_buffer.pushPacket(nullptr);
            } catch (const std::invalid_argument&) {
                rejected_null_packet = true;
            }
            if (!rejected_null_packet || encoded_video_buffer.getPacketCount() != 0) {
                throw std::runtime_error("O buffer deve rejeitar pacote nulo sem alterar a contagem.");
            }
            std::cout << "PASS: pacote nulo rejeitado.\n";
        }

        {
            EncodedVideoBuffer encoded_video_buffer;
            const AVPacket* first_packet_address = nullptr;
            for (std::int64_t packet_index = 0; packet_index < 3; ++packet_index) {
                AvPacketPtr encoded_packet = createIndexedPacket(packet_index);
                if (packet_index == 0) {
                    first_packet_address = encoded_packet.get();
                }
                encoded_video_buffer.pushPacket(std::move(encoded_packet));
                if (encoded_video_buffer.getPacketCount() != static_cast<std::size_t>(packet_index + 1)) {
                    throw std::runtime_error("Cada pushPacket deve aumentar a contagem em um.");
                }
            }

            for (std::int64_t packet_index = 0; packet_index < 3; ++packet_index) {
                AvPacketPtr encoded_packet = encoded_video_buffer.tryPopPacket();
                if (!encoded_packet || encoded_packet->pts != packet_index ||
                    (packet_index == 0 && encoded_packet.get() != first_packet_address)) {
                    throw std::runtime_error("Os pacotes devem sair na ordem de entrada, sem copia do AVPacket.");
                }
                if (encoded_video_buffer.getPacketCount() != static_cast<std::size_t>(2 - packet_index)) {
                    throw std::runtime_error("Cada tryPopPacket deve reduzir a contagem em um.");
                }
            }
            if (encoded_video_buffer.tryPopPacket() != nullptr) {
                throw std::runtime_error("Depois de retirar tudo, o buffer deve devolver nullptr.");
            }
            std::cout << "PASS: ordem FIFO, contagem e transferencia de posse sem copia.\n";
        }

        {
            constexpr std::int64_t produced_packet_count = 10000;
            EncodedVideoBuffer encoded_video_buffer;

            // Simula a thread interna do OpenCV empurrando pacotes enquanto o main retira.
            std::thread producer_thread([&encoded_video_buffer]() {
                for (std::int64_t packet_index = 0; packet_index < produced_packet_count; ++packet_index) {
                    encoded_video_buffer.pushPacket(createIndexedPacket(packet_index));
                }
            });

            std::int64_t expected_packet_index = 0;
            bool order_preserved = true;
            while (expected_packet_index < produced_packet_count) {
                AvPacketPtr encoded_packet = encoded_video_buffer.tryPopPacket();
                if (!encoded_packet) {
                    std::this_thread::yield();
                    continue;
                }
                if (encoded_packet->pts != expected_packet_index) {
                    order_preserved = false;
                }
                ++expected_packet_index;
            }
            producer_thread.join();

            if (!order_preserved || encoded_video_buffer.getPacketCount() != 0 ||
                encoded_video_buffer.tryPopPacket() != nullptr) {
                throw std::runtime_error("Com duas threads, todos os pacotes devem sair uma vez e em ordem.");
            }
            std::cout << "PASS: produtor e consumidor em threads diferentes, " << produced_packet_count
                      << " pacotes em ordem e sem perda.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
