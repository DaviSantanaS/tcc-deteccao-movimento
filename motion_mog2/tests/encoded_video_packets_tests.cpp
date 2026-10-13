#include "EncodedVideoPackets.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

int main() {
    try {
        EncodedVideoPackets empty_encoded_video_packets;
        if (!empty_encoded_video_packets.encoded_packets.empty() ||
            empty_encoded_video_packets.video_stream_parameters) {
            throw std::runtime_error("A colecao padrao deve estar vazia e sem parametros.");
        }

        std::weak_ptr<const VideoStreamParameters> stream_parameters_lifetime;
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

            auto first_packet = allocateAvPacket();
            auto second_packet = allocateAvPacket();
            if (av_new_packet(first_packet.get(), 1) < 0 ||
                av_new_packet(second_packet.get(), 1) < 0) {
                throw std::runtime_error("Nao foi possivel alocar os dados dos pacotes de teste.");
            }

            first_packet->data[0] = 42;
            first_packet->pts = 123;
            first_packet->dts = 120;
            first_packet->flags = AV_PKT_FLAG_KEY;
            first_packet->time_base = AVRational{1, 90000};
            second_packet->data[0] = 84;
            second_packet->pts = 124;
            second_packet->dts = 121;
            second_packet->time_base = AVRational{1, 90000};

            const AVPacket* first_packet_address = first_packet.get();
            const AVPacket* second_packet_address = second_packet.get();
            const auto* first_packet_data_address = first_packet->data;
            const auto* second_packet_data_address = second_packet->data;

            std::vector<AvPacketPtr> pending_packets;
            pending_packets.push_back(std::move(first_packet));
            pending_packets.push_back(std::move(second_packet));

            EncodedVideoPackets encoded_video_packets{
                std::move(pending_packets), video_stream_parameters
            };
            EncodedVideoPackets another_encoded_video_packets{
                {}, video_stream_parameters
            };

            if (encoded_video_packets.encoded_packets.size() != 2 ||
                encoded_video_packets.encoded_packets[0].get() != first_packet_address ||
                encoded_video_packets.encoded_packets[1].get() != second_packet_address) {
                throw std::runtime_error("A transferencia do vetor deve preservar os pacotes e sua ordem.");
            }

            EncodedVideoPackets moved_encoded_video_packets{std::move(encoded_video_packets)};
            if (moved_encoded_video_packets.encoded_packets.size() != 2 ||
                moved_encoded_video_packets.encoded_packets[0].get() != first_packet_address ||
                moved_encoded_video_packets.encoded_packets[1].get() != second_packet_address) {
                throw std::runtime_error("Mover a colecao deve preservar os pacotes e sua ordem.");
            }

            const auto& retained_first_packet = moved_encoded_video_packets.encoded_packets[0];
            const auto& retained_second_packet = moved_encoded_video_packets.encoded_packets[1];
            if (retained_first_packet->data != first_packet_data_address ||
                retained_second_packet->data != second_packet_data_address ||
                retained_first_packet->size != 1 || retained_second_packet->size != 1 ||
                retained_first_packet->data[0] != 42 || retained_second_packet->data[0] != 84 ||
                retained_first_packet->pts != 123 || retained_second_packet->pts != 124 ||
                retained_first_packet->dts != 120 || retained_second_packet->dts != 121 ||
                retained_first_packet->flags != AV_PKT_FLAG_KEY || retained_second_packet->flags != 0 ||
                retained_first_packet->time_base.num != 1 || retained_first_packet->time_base.den != 90000 ||
                retained_second_packet->time_base.num != 1 || retained_second_packet->time_base.den != 90000) {
                throw std::runtime_error("Os dados e metadados dos pacotes devem permanecer intactos.");
            }

            if (moved_encoded_video_packets.video_stream_parameters.get() != video_stream_parameters.get() ||
                another_encoded_video_packets.video_stream_parameters.get() != video_stream_parameters.get() ||
                encoded_video_packets.video_stream_parameters) {
                throw std::runtime_error("As colecoes devem compartilhar a mesma configuracao da stream.");
            }

            video_stream_parameters.reset();
            another_encoded_video_packets.video_stream_parameters.reset();
            if (stream_parameters_lifetime.expired() ||
                moved_encoded_video_packets.video_stream_parameters->codec_parameters->width != 1920) {
                throw std::runtime_error("A configuracao deve continuar viva enquanto uma colecao a possui.");
            }
        }

        if (!stream_parameters_lifetime.expired()) {
            throw std::runtime_error("A configuracao deve ser liberada apos sua ultima referencia.");
        }

        std::cout << "PASS: colecao de pacotes movida sem copias e configuracao compartilhada com vida util segura.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
