#include "EncodedBytesDiagnostics.hpp"

#include <opencv2/core/cuda.hpp>
#include <opencv2/cudacodec.hpp>

#include <cstddef>
#include <iostream>
#include <stdexcept>

int main(int argument_count, char** argument_values) {
    if (argument_count != 2) {
        std::cerr << "Uso: opencv_nvdec_file_tests <arquivo-local-h264>\n";
        return 2;
    }

    try {
        if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
            std::cerr << "SKIP: nenhuma GPU CUDA disponivel para testar NVDEC.\n";
            return 77;
        }
        cv::cuda::setDevice(0);

        cv::cudacodec::VideoReaderInitParams reader_parameters;
        reader_parameters.rawMode = true;
        reader_parameters.allowFrameDrop = false;
        reader_parameters.udpSource = true;

        cv::cuda::Stream cuda_stream;
        auto video_reader = cv::cudacodec::createVideoReader(argument_values[1], {}, reader_parameters);
        cv::cuda::GpuMat decoded_frame_gpu;
        constexpr int expected_frame_count = 3;
        bool encoded_packet_inspection_complete = false;

        for (int decoded_frame_index = 0; decoded_frame_index < expected_frame_count; ++decoded_frame_index) {
            if (!video_reader->nextFrame(decoded_frame_gpu, cuda_stream)) {
                throw std::runtime_error("O leitor padrao deve fornecer pelo menos tres frames.");
            }
            cuda_stream.waitForCompletion();
            if (decoded_frame_gpu.empty()) {
                throw std::runtime_error("O leitor padrao retornou uma imagem vazia na GPU.");
            }
            if (encoded_packet_inspection_complete) {
                continue;
            }

            double raw_packet_count = 0;
            if (!video_reader->get(
                cv::cudacodec::VideoReaderProps::PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB,
                raw_packet_count
            )) {
                throw std::runtime_error("Nao foi possivel consultar a quantidade de pacotes raw.");
            }
            if (raw_packet_count == 0) {
                continue;
            }

            double raw_packet_base_index = 0;
            double extra_data_index = 0;
            if (!video_reader->get(
                cv::cudacodec::VideoReaderProps::PROP_RAW_PACKAGES_BASE_INDEX,
                raw_packet_base_index
            ) || !video_reader->get(
                cv::cudacodec::VideoReaderProps::PROP_EXTRA_DATA_INDEX,
                extra_data_index
            )) {
                throw std::runtime_error("Nao foi possivel consultar os indices de pacote raw e extradata.");
            }

            cv::Mat encoded_packet;
            if (!video_reader->retrieve(encoded_packet, static_cast<std::size_t>(raw_packet_base_index))) {
                throw std::runtime_error("O pacote raw anunciado pelo leitor nao pode ser recuperado.");
            }
            printEncodedBytesPrefix(
                "opencv_decoder_packet", encoded_packet.data, encoded_packet.total() * encoded_packet.elemSize()
            );

            cv::Mat codec_extra_data;
            video_reader->retrieve(codec_extra_data, static_cast<std::size_t>(extra_data_index));
            printEncodedBytesPrefix(
                "opencv_extradata", codec_extra_data.data, codec_extra_data.total() * codec_extra_data.elemSize()
            );
            encoded_packet_inspection_complete = true;
        }

        if (!encoded_packet_inspection_complete) {
            throw std::runtime_error("Nenhum pacote raw foi observado durante a leitura dos tres frames.");
        }

        std::cout << "PASS: leitor padrao do OpenCV decodificou " << expected_frame_count
                  << " frames via NVDEC; width=" << decoded_frame_gpu.cols
                  << " height=" << decoded_frame_gpu.rows << '\n';
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
