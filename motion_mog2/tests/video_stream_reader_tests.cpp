#include "VideoStreamReader.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Uso: video_stream_reader_tests <arquivo-local-h264>\n";
        return 2;
    }

    try {
        if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
            std::cerr << "SKIP: nenhuma GPU CUDA disponivel para testar NVDEC.\n";
            return 77;
        }
        cv::cuda::setDevice(0);

        std::vector<AvPacketPtr> retained_encoded_packets;
        std::shared_ptr<const VideoStreamParameters> retained_video_stream_parameters;
        std::weak_ptr<const VideoStreamParameters> video_stream_parameters_lifetime;
        std::weak_ptr<const AVCodecParameters> codec_parameters_lifetime;
        const AVPacket* retained_packet_address = nullptr;
        const unsigned char* retained_packet_data_address = nullptr;
        std::vector<unsigned char> retained_packet_contents;
        std::size_t received_packet_count = 0;

        {
            cv::cuda::Stream cuda_stream;
            VideoStreamReader video_reader(argv[1]);
            const VideoStreamReader& const_video_reader = video_reader;
            retained_video_stream_parameters = const_video_reader.videoStreamParameters();
            const auto second_video_stream_parameters = const_video_reader.videoStreamParameters();
            if (!retained_video_stream_parameters || !retained_video_stream_parameters->codec_parameters ||
                retained_video_stream_parameters.get() != second_video_stream_parameters.get()) {
                throw std::runtime_error("O getter deve compartilhar uma unica configuracao valida.");
            }
            video_stream_parameters_lifetime = retained_video_stream_parameters;
            codec_parameters_lifetime = retained_video_stream_parameters->codec_parameters;

            if (!std::isfinite(video_reader.fps()) || video_reader.fps() <= 0.0) {
                throw std::runtime_error("A inicializacao deve fornecer um FPS finito e positivo.");
            }
            std::cout << "reader FPS=" << video_reader.fps() << '\n';
            DecodedFrame decoded_frame;
            std::vector<AvPacketPtr> encoded_packets;

            for (std::uint64_t expected_frame_index = 0; expected_frame_index < 3; ++expected_frame_index) {
                if (!video_reader.read(decoded_frame, encoded_packets, cuda_stream)) {
                    throw std::runtime_error("O arquivo deve fornecer pelo menos tres frames decodificados.");
                }
                cuda_stream.waitForCompletion();

                if (!std::isfinite(video_reader.fps()) || video_reader.fps() <= 0.0) {
                    throw std::runtime_error("O FPS deve permanecer finito e positivo durante as leituras.");
                }

                if (decoded_frame.decoded_frame_gpu.empty() ||
                    decoded_frame.decoded_frame_index != expected_frame_index ||
                    video_reader.decodedFrameCount() != expected_frame_index + 1) {
                    throw std::runtime_error("A leitura deve produzir imagem na GPU, indice e contador corretos.");
                }

                const auto stream_parameters = const_video_reader.videoStreamParameters();
                if (!stream_parameters || !stream_parameters->codec_parameters ||
                    stream_parameters->codec_parameters->codec_id != AV_CODEC_ID_H264 ||
                    stream_parameters->codec_parameters->width <= 0 ||
                    stream_parameters->codec_parameters->height <= 0 ||
                    stream_parameters->packet_timestamp_time_base.num <= 0 ||
                    stream_parameters->packet_timestamp_time_base.den <= 0) {
                    throw std::runtime_error("A configuracao da stream deve permanecer valida durante as leituras.");
                }
                if (stream_parameters.get() != retained_video_stream_parameters.get() ||
                    stream_parameters.owner_before(retained_video_stream_parameters) ||
                    retained_video_stream_parameters.owner_before(stream_parameters)) {
                    throw std::runtime_error("O getter deve compartilhar a posse da mesma configuracao durante as leituras.");
                }

                received_packet_count += encoded_packets.size();
                for (const auto& packet : encoded_packets) {
                    if (!packet || packet->data == nullptr || packet->size <= 0 ||
                        packet->time_base.num != stream_parameters->packet_timestamp_time_base.num ||
                        packet->time_base.den != stream_parameters->packet_timestamp_time_base.den) {
                        throw std::runtime_error("Os pacotes retornados devem possuir dados e base de tempo validos.");
                    }
                }

                if (retained_encoded_packets.empty() && !encoded_packets.empty()) {
                    const auto& first_packet = encoded_packets.front();
                    retained_packet_address = first_packet.get();
                    retained_packet_data_address = first_packet->data;
                    retained_packet_contents.assign(first_packet->data, first_packet->data + first_packet->size);
                    retained_encoded_packets = std::move(encoded_packets);
                }
            }

            if (received_packet_count == 0 || retained_encoded_packets.empty()) {
                throw std::runtime_error("O conjunto das tres leituras deve fornecer algum pacote codificado.");
            }
            cuda_stream.waitForCompletion();
        }

        const auto& retained_packet = retained_encoded_packets.front();
        if (video_stream_parameters_lifetime.expired() || codec_parameters_lifetime.expired() ||
            retained_video_stream_parameters->codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            retained_video_stream_parameters->codec_parameters->width <= 0 ||
            retained_packet.get() != retained_packet_address ||
            retained_packet->data != retained_packet_data_address ||
            static_cast<std::size_t>(retained_packet->size) != retained_packet_contents.size() ||
            std::memcmp(retained_packet->data, retained_packet_contents.data(), retained_packet_contents.size()) != 0) {
            throw std::runtime_error("Mover o vetor e destruir o leitor nao deve invalidar pacotes ou configuracao.");
        }

        retained_encoded_packets.clear();
        if (video_stream_parameters_lifetime.expired() || codec_parameters_lifetime.expired() ||
            retained_video_stream_parameters->codec_parameters->codec_id != AV_CODEC_ID_H264 ||
            retained_video_stream_parameters->codec_parameters->width <= 0 ||
            retained_video_stream_parameters->codec_parameters->height <= 0 ||
            retained_video_stream_parameters->packet_timestamp_time_base.num <= 0 ||
            retained_video_stream_parameters->packet_timestamp_time_base.den <= 0) {
            throw std::runtime_error("O ponteiro do getter deve preservar os parametros sem o reader e os pacotes.");
        }

        retained_video_stream_parameters.reset();
        if (!video_stream_parameters_lifetime.expired() || !codec_parameters_lifetime.expired()) {
            throw std::runtime_error("A ultima referencia compartilhada deve liberar a configuracao preservada.");
        }

        std::cout << "PASS: tres frames via NVDEC, " << received_packet_count
                  << " pacotes com configuracao compartilhada e vida util segura.\n";
        std::cout << "PASS: getter compartilha a posse dos parametros, preservados sem o reader e liberados ao final.\n";
        return 0;
    } catch (const cv::Exception& error) {
        if (error.code == cv::Error::GpuNotSupported || error.code == cv::Error::StsNotImplemented) {
            std::cerr << "SKIP: CUDA/NVDEC indisponivel: " << error.what() << '\n';
            return 77;
        }
        std::cerr << "FAIL: CUDA/NVDEC: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
