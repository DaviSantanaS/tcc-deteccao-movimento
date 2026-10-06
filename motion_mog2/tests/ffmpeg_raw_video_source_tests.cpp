#include "FfmpegRawVideoSource.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Uso: ffmpeg_raw_video_source_tests <entrada-de-video>\n";
        return 2;
    }

    try {
        FfmpegRawVideoSource valid_source(argv[1]);
        if (valid_source.format().codec != cv::cudacodec::Codec::H264) {
            std::cerr << "O codec da entrada valida nao foi identificado como H.264.\n";
            return 1;
        }

        cv::Mat codec_extra_data;
        valid_source.getExtraData(codec_extra_data);
        if (codec_extra_data.empty() ||
            codec_extra_data.type() != CV_8UC1 ||
            codec_extra_data.total() != 35) {
            std::cerr << "A extradata H.264 nao possui os 35 bytes esperados.\n";
            return 1;
        }

        const unsigned char original_first_byte = codec_extra_data.data[0];
        codec_extra_data.data[0] ^= 0xFF;

        cv::Mat second_extra_data_copy;
        valid_source.getExtraData(second_extra_data_copy);
        if (second_extra_data_copy.data[0] != original_first_byte) {
            std::cerr << "getExtraData nao retornou uma copia independente.\n";
            return 1;
        }

        std::size_t first_packet_size = 0;
        unsigned char first_packet_first_byte = 0;

        for (int packet_number = 1; packet_number <= 3; ++packet_number) {
            unsigned char* packet_data = nullptr;
            std::size_t packet_size = 0;

            if (!valid_source.getNextPacket(&packet_data, &packet_size)) {
                std::cerr << "Nao foi possivel obter o pacote de video "
                          << packet_number << ".\n";
                return 1;
            }

            if (packet_data == nullptr || packet_size == 0) {
                std::cerr << "O pacote de video retornado esta vazio.\n";
                return 1;
            }

            if (packet_number == 1) {
                first_packet_size = packet_size;
                first_packet_first_byte = packet_data[0];
            }

            std::cout << "packet size=" << packet_size << '\n';
        }

        std::vector<AvPacketPtr> pending_packets =
            valid_source.takePendingPackets(3);
        if (pending_packets.size() != 3) {
            std::cerr << "A quantidade de pacotes pendentes esta incorreta.\n";
            return 1;
        }

        for (const AvPacketPtr& pending_packet : pending_packets) {
            if (!pending_packet ||
                pending_packet->data == nullptr ||
                pending_packet->size <= 0) {
                std::cerr << "A fila retornou um AVPacket vazio.\n";
                return 1;
            }
        }

        if (static_cast<std::size_t>(pending_packets.front()->size) !=
                first_packet_size ||
            pending_packets.front()->data[0] != first_packet_first_byte) {
            std::cerr << "O primeiro clone nao sobreviveu a reutilizacao do pacote.\n";
            return 1;
        }

        try {
            valid_source.takePendingPackets(1);
            std::cerr << "A fila vazia aceitou a retirada de outro pacote.\n";
            return 1;
        } catch (const std::runtime_error&) {
        }
    } catch (const std::exception& error) {
        std::cerr << "Falha ao abrir a entrada valida: " << error.what() << "\n";
        return 1;
    }

    try {
        FfmpegRawVideoSource invalid_source(
            "/tmp/tcc-entrada-de-video-inexistente.mp4"
        );
    } catch (const std::exception&) {
        return 0;
    }

    std::cerr << "A entrada inexistente foi aceita.\n";
    return 1;
}
