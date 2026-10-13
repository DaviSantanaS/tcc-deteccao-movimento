#include "FfmpegRawVideoSource.hpp"

#include <cstring>
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

        AVFormatContext* reference_input = nullptr;
        const int reference_open_status = avformat_open_input(&reference_input, argv[1], nullptr, nullptr);
        const auto close_reference_input = [](AVFormatContext* input) { avformat_close_input(&input); };
        const std::unique_ptr<AVFormatContext, decltype(close_reference_input)> reference_context(
            reference_input, close_reference_input
        );
        if (reference_open_status < 0 || avformat_find_stream_info(reference_context.get(), nullptr) < 0) {
            throw std::runtime_error("Nao foi possivel abrir a entrada independente de referencia.");
        }
        const int reference_video_stream_index = av_find_best_stream(
            reference_context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
        );
        if (reference_video_stream_index < 0) {
            throw std::runtime_error("A entrada independente deve fornecer uma stream de video.");
        }
        auto reference_packet = allocateAvPacket();
        while (true) {
            if (av_read_frame(reference_context.get(), reference_packet.get()) < 0) {
                throw std::runtime_error("Nao foi possivel ler o pacote original de referencia.");
            }
            if (reference_packet->stream_index == reference_video_stream_index &&
                reference_packet->data != nullptr && reference_packet->size > 0) {
                break;
            }
            av_packet_unref(reference_packet.get());
        }

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

            if (packet_size < 3 || packet_data[0] != 0 || packet_data[1] != 0 ||
                (packet_data[2] != 1 && (packet_size < 4 || packet_data[2] != 0 || packet_data[3] != 1))) {
                std::cerr << "O decoder deve receber um pacote em Annex B.\n";
                return 1;
            }

            std::cout << "decoder packet size=" << packet_size << '\n';
        }

        std::vector<AvPacketPtr> pending_packets =
            valid_source.dequeueEncodedPackets(3);
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

        if (pending_packets.front()->size != reference_packet->size ||
            pending_packets.front()->pts != reference_packet->pts || pending_packets.front()->dts != reference_packet->dts ||
            std::memcmp(pending_packets.front()->data, reference_packet->data,
                static_cast<std::size_t>(reference_packet->size)) != 0) {
            std::cerr << "O primeiro clone nao preservou o pacote original apos as proximas leituras.\n";
            return 1;
        }

        try {
            valid_source.dequeueEncodedPackets(1);
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
