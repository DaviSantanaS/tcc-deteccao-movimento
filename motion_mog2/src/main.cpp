#include "AvOutputFormatContext.hpp"
#include "AvOutputIoContext.hpp"
#include "EncodedVideoBuffer.hpp"
#include "VideoStreamReader.hpp"

#include <opencv2/core/cuda.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

std::atomic<bool> keep_running{true};

void handle_signal(int) {
    keep_running = false;
}

int main(int argc, char** argv) {
    const bool show_help = argc == 2 &&
        (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h");
    if (show_help || argc > 4) {
        std::cout << "Uso: motion_mog2 [fonte] [diretorio_saida] [leituras_por_trecho]\n"
                     "Padrao: rtsp://127.0.0.1:8554/video ./recordings 1000\n"
                     "O corte ocorre no proximo keyframe apos atingir o alvo de leituras.\n";
        return show_help ? 0 : 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    try {
        const std::string input_source =
            (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
        const std::filesystem::path output_directory = std::filesystem::absolute(
            argc > 2 ? argv[2] : "./recordings"
        );
        const std::string requested_read_count = argc > 3 ? argv[3] : "1000";
        if (requested_read_count.empty() ||
            !std::all_of(requested_read_count.begin(), requested_read_count.end(),
                [](unsigned char character) { return character >= '0' && character <= '9'; })) {
            throw std::invalid_argument("O alvo de leituras deve ser um inteiro positivo.");
        }
        const auto parsed_read_count = std::stoull(requested_read_count);
        if (parsed_read_count == 0 || parsed_read_count > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("O alvo de leituras deve caber em size_t e ser maior que zero.");
        }
        const auto target_decoded_frame_read_count = static_cast<std::size_t>(parsed_read_count);

        if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
            std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
            return 1;
        }
        cv::cuda::setDevice(0);

        VideoStreamReader video_reader(input_source);
        const auto video_stream_parameters = video_reader.videoStreamParameters();
        if (!video_stream_parameters) {
            throw std::invalid_argument("Os parametros da stream devem estar disponiveis apos construir o reader.");
        }
        EncodedVideoBuffer encoded_video_buffer(target_decoded_frame_read_count);
        std::filesystem::create_directories(output_directory);

        std::cout << "STREAM_FPS fps=" << video_reader.fps() << "\n";
        std::cout << "STREAM_RESOLUTION width=" << video_reader.width()
                  << " height=" << video_reader.height() << "\n";
        std::cout << "MP4_RECORDING target_decoded_reads=" << target_decoded_frame_read_count
                  << " directory=" << output_directory << "\n";
        std::cout.flush();

        cv::cuda::Stream cuda_stream;
        DecodedFrame decoded_frame;
        std::vector<AvPacketPtr> encoded_packets;

        // O contexto MP4 deve ser destruido antes do I/O que ele referencia.
        AvOutputIoContextPtr output_io_context;
        AvOutputFormatContextPtr mp4_output_context;
        std::filesystem::path current_output_file_path;
        std::size_t recording_decoded_read_count = 0;
        std::size_t recording_encoded_packet_count = 0;
        std::size_t skipped_initial_packet_count = 0;
        std::size_t completed_recording_count = 0;
        bool waiting_for_next_keyframe = false;

        const auto write_encoded_packets = [&](std::vector<AvPacketPtr>&& encoded_packets_to_write) {
            writeMp4Packets(*mp4_output_context, std::move(encoded_packets_to_write), video_stream_parameters);
        };
        const auto finish_recording = [&]() {
            writeMp4Packets(*mp4_output_context, encoded_video_buffer.takeBufferedEncodedPackets(), video_stream_parameters);
            writeMp4Trailer(*mp4_output_context);
            mp4_output_context.reset();
            output_io_context.reset();
            ++completed_recording_count;
            std::cout << "MP4_SAVED path=" << current_output_file_path
                      << " decoded_reads=" << recording_decoded_read_count
                      << " encoded_packets=" << recording_encoded_packet_count << "\n";
            std::cout.flush();
        };

        while (keep_running) {
            if (!video_reader.read(decoded_frame, encoded_packets, cuda_stream)) {
                break;
            }
            cuda_stream.waitForCompletion();

            for (const auto& packet : encoded_packets) {
                if (!packet) {
                    throw std::invalid_argument("A leitura retornou um AVPacket nulo.");
                }
            }

            if (!mp4_output_context || waiting_for_next_keyframe) {
                auto next_keyframe = std::find_if(
                    encoded_packets.begin(),
                    encoded_packets.end(),
                    [](const AvPacketPtr& packet) { return (packet->flags & AV_PKT_FLAG_KEY) != 0; }
                );
                if (!mp4_output_context && next_keyframe == encoded_packets.end()) {
                    skipped_initial_packet_count += encoded_packets.size();
                    continue;
                }
                if (next_keyframe != encoded_packets.end()) {
                    if (mp4_output_context) {
                        std::vector<AvPacketPtr> packets_before_keyframe;
                        for (auto packet = encoded_packets.begin(); packet != next_keyframe; ++packet) {
                            packets_before_keyframe.push_back(std::move(*packet));
                        }
                        recording_encoded_packet_count += packets_before_keyframe.size();
                        write_encoded_packets(std::move(packets_before_keyframe));
                        finish_recording();
                    } else {
                        skipped_initial_packet_count += static_cast<std::size_t>(
                            next_keyframe - encoded_packets.begin()
                        );
                    }
                    encoded_packets.erase(
                        encoded_packets.begin(), next_keyframe
                    );
                }
            }

            if (!mp4_output_context) {
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
                recording_decoded_read_count = 0;
                recording_encoded_packet_count = 0;
                waiting_for_next_keyframe = false;
                std::cout << "MP4_STARTED path=" << current_output_file_path << "\n";
                std::cout.flush();
            }

            ++recording_decoded_read_count;
            recording_encoded_packet_count += encoded_packets.size();
            if (waiting_for_next_keyframe) {
                write_encoded_packets(std::move(encoded_packets));
            } else if (encoded_video_buffer.bufferEncodedPackets(std::move(encoded_packets))) {
                writeMp4Packets(*mp4_output_context, encoded_video_buffer.takeBufferedEncodedPackets(), video_stream_parameters);
                waiting_for_next_keyframe = true;
            }
        }

        if (mp4_output_context) {
            finish_recording();
        }
        std::cout << "CAPTURE_COMPLETE decoded_reads=" << video_reader.processedFrameCount()
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
