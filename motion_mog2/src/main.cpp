#include "ffmpeg/AvMp4OutputContext.hpp"
#include "ffmpeg/AvOutputFileContext.hpp"
#include "buffers/EncodedVideoBuffer.hpp"
#include "input/VideoStreamReader.hpp"

#include <opencv2/core/cuda.hpp>
#include <opencv2/highgui.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

std::atomic<bool> capture_active{true};

void handleTerminationSignal(int) {
    ::capture_active = false;
}

int main(int argument_count, char** argument_values) {
    const bool help_requested = argument_count == 2 &&
        (std::string(argument_values[1]) == "--help" || std::string(argument_values[1]) == "-h");
    if (help_requested || argument_count > 5) {
        std::cout << "Uso: motion_mog2 [fonte] [diretorio_saida] [pacotes_por_trecho] [--sem-video]\n"
                     "Padrao: rtsp://127.0.0.1:8554/video ./recordings 1000\n"
                     "O corte ocorre no proximo keyframe apos atingir o alvo de pacotes.\n"
                     "--sem-video desativa a janela com o video decodificado (Esc ou q encerram).\n";
        return help_requested ? 0 : 1;
    }

    signal(SIGINT, handleTerminationSignal);
    signal(SIGTERM, handleTerminationSignal);

    try {
        const std::string input_source =
            (argument_count > 1) ? argument_values[1] : "rtsp://127.0.0.1:8554/video";
        const std::filesystem::path output_directory = std::filesystem::absolute(
            argument_count > 2 ? argument_values[2] : "./recordings"
        );
        const std::string requested_packet_count_text = argument_count > 3 ? argument_values[3] : "1000";
        if (requested_packet_count_text.empty() ||
            !std::all_of(requested_packet_count_text.begin(), requested_packet_count_text.end(),
                [](unsigned char character) { return character >= '0' && character <= '9'; })) {
            throw std::invalid_argument("O alvo de pacotes deve ser um inteiro positivo.");
        }
        const auto parsed_packet_count = std::stoull(requested_packet_count_text);
        if (parsed_packet_count == 0 || parsed_packet_count > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("O alvo de pacotes deve caber em size_t e ser maior que zero.");
        }
        const auto target_recording_packet_count = static_cast<std::size_t>(parsed_packet_count);
        if (argument_count > 4 && std::string(argument_values[4]) != "--sem-video") {
            throw std::invalid_argument("O quarto argumento, se presente, deve ser --sem-video.");
        }
        const bool preview_enabled = argument_count <= 4;

        if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
            std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
            return 1;
        }
        cv::cuda::setDevice(0);

        // Compartilhado com a fonte FFmpeg, que o usa na thread interna do OpenCV
        // enquanto o VideoReader existir.
        const auto encoded_video_buffer = std::make_shared<EncodedVideoBuffer>();
        VideoStreamReader video_reader(input_source, encoded_video_buffer);
        const auto video_stream_parameters = video_reader.getVideoStreamParameters();
        if (!video_stream_parameters) {
            throw std::invalid_argument("Os parametros da stream devem estar disponiveis apos construir o reader.");
        }
        std::filesystem::create_directories(output_directory);

        std::cout << "STREAM_FPS fps=" << video_reader.getFramesPerSecond() << "\n";
        std::cout << "STREAM_RESOLUTION width=" << video_reader.getWidth()
                  << " height=" << video_reader.getHeight() << "\n";
        std::cout << "MP4_RECORDING target_encoded_packets=" << target_recording_packet_count
                  << " directory=" << output_directory << "\n";
        std::cout.flush();

        cv::cuda::Stream cuda_stream;
        DecodedFrame decoded_frame;

        AvOutputFileContextPtr output_io_context;
        AvMp4OutputContextPtr mp4_output_context;
        std::filesystem::path current_output_file_path;
        std::size_t recording_encoded_packet_count = 0;
        std::size_t skipped_initial_packet_count = 0;
        std::size_t completed_recording_count = 0;
        bool next_keyframe_boundary_pending = false;

        const auto recording_starter = [&]() {
            const std::time_t recording_start_time = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()
            );
            std::tm local_recording_start{};
            if (!localtime_r(&recording_start_time, &local_recording_start)) {
                throw std::runtime_error("Nao foi possivel obter o horario local da gravacao.");
            }
            constexpr std::array<const char*, 12> portuguese_month_names{
                "jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"
            };
            std::ostringstream recording_file_name;
            recording_file_name << std::put_time(&local_recording_start, "%d_")
                                << portuguese_month_names.at(local_recording_start.tm_mon)
                                << std::put_time(&local_recording_start, "_%y_%H.%M.%S");
            const std::string recording_file_stem = recording_file_name.str();
            current_output_file_path = output_directory / (recording_file_stem + ".mp4");
            for (std::size_t suffix = 1;
                 std::filesystem::symlink_status(current_output_file_path).type() != std::filesystem::file_type::not_found;
                 ++suffix) {
                std::ostringstream unique_file_name;
                unique_file_name << recording_file_stem << '_' << std::setfill('0') << std::setw(3)
                                 << suffix << ".mp4";
                current_output_file_path = output_directory / unique_file_name.str();
            }

            mp4_output_context = allocateMp4OutputContext();
            mp4_output_context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
            (void)createMp4VideoStream(*mp4_output_context, *video_stream_parameters);
            output_io_context = openOutputFile(current_output_file_path);
            writeMp4Header(*mp4_output_context, output_io_context);
            recording_encoded_packet_count = 0;
            next_keyframe_boundary_pending = false;
            std::cout << "MP4_STARTED path=" << current_output_file_path << "\n";
            std::cout.flush();
        };
        const auto recording_finalizer = [&]() {
            writeMp4Trailer(*mp4_output_context);
            mp4_output_context.reset();
            output_io_context.reset();
            ++completed_recording_count;
            std::cout << "MP4_SAVED path=" << current_output_file_path
                      << " encoded_packets=" << recording_encoded_packet_count << "\n";
            std::cout.flush();
        };

        // Politica de gravacao aplicada a um pacote por vez, na ordem de chegada.
        // Cada MP4 comeca em keyframe; ao atingir o alvo, o corte espera o proximo keyframe.
        const auto encoded_packet_recorder = [&](AvPacketPtr encoded_packet) {
            const bool keyframe_packet = (encoded_packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (!mp4_output_context) {
                if (!keyframe_packet) {
                    ++skipped_initial_packet_count;
                    return;
                }
                recording_starter();
            } else if (next_keyframe_boundary_pending && keyframe_packet) {
                recording_finalizer();
                recording_starter();
            }

            // O muxer MP4 pode ajustar a base de tempo da stream em writeMp4Header.
            const AVStream& mp4_video_stream = *mp4_output_context->streams[0];
            prepareEncodedPacketForOutput(
                *encoded_packet,
                video_stream_parameters->packet_timestamp_time_base,
                mp4_video_stream.time_base,
                mp4_video_stream.index
            );
            writeMp4Packet(*mp4_output_context, *encoded_packet);

            ++recording_encoded_packet_count;
            if (recording_encoded_packet_count >= target_recording_packet_count) {
                next_keyframe_boundary_pending = true;
            }
        };
        const auto buffered_packets_recorder = [&]() {
            for (AvPacketPtr encoded_packet = encoded_video_buffer->tryPopPacket();
                 encoded_packet;
                 encoded_packet = encoded_video_buffer->tryPopPacket()) {
                encoded_packet_recorder(std::move(encoded_packet));
            }
        };

        // Exibe o mesmo frame decodificado pelo NVDEC, sem segunda conexao RTSP.
        // Com OpenGL, imshow aceita GpuMat direto; sem OpenGL, o frame e copiado para a CPU.
        const std::string preview_window_name = "TCC - RTSP e gravacao MP4";
        bool preview_uses_opengl = false;
        cv::Mat decoded_frame_cpu;
        if (preview_enabled) {
            try {
                cv::namedWindow(preview_window_name, cv::WINDOW_OPENGL | cv::WINDOW_AUTOSIZE);
                preview_uses_opengl = true;
            } catch (const cv::Exception& error) {
                std::cerr << "[aviso] Janela sem OpenGL; o frame sera copiado da GPU para a CPU: "
                          << error.err << "\n";
                cv::namedWindow(preview_window_name, cv::WINDOW_AUTOSIZE);
            }
            std::cout << "PREVIEW opengl=" << (preview_uses_opengl ? 1 : 0) << "\n";
            std::cout.flush();
        }
        const auto decoded_frame_presenter = [&]() {
            if (preview_uses_opengl) {
                cv::imshow(preview_window_name, decoded_frame.decoded_frame_gpu);
            } else {
                decoded_frame.decoded_frame_gpu.download(decoded_frame_cpu);
                cv::imshow(preview_window_name, decoded_frame_cpu);
            }
            const int pressed_key = cv::waitKey(1);
            if (pressed_key == 27 || pressed_key == 'q') {
                ::capture_active = false;
            }
        };

        while (::capture_active) {
            if (!video_reader.read(decoded_frame, cuda_stream)) {
                break;
            }
            cuda_stream.waitForCompletion();

            if (preview_enabled) {
                decoded_frame_presenter();
            }
            buffered_packets_recorder();
        }

        // Pacotes lidos pela fonte depois do ultimo frame entregue ainda estao no buffer.
        buffered_packets_recorder();
        if (mp4_output_context) {
            recording_finalizer();
        }
        if (preview_enabled) {
            cv::destroyWindow(preview_window_name);
        }
        std::cout << "CAPTURE_COMPLETE decoded_reads=" << video_reader.getDecodedFrameCount()
                  << " saved_files=" << completed_recording_count
                  << " skipped_initial_packets=" << skipped_initial_packet_count << "\n";
    } catch (const cv::Exception& error) {
        std::cerr << "[fatal] CUDA/NVDEC: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << "\n";
        return 2;
    }

    return 0;
}
