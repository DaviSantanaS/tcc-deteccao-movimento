#include "ffmpeg/AvMp4OutputContext.hpp"
#include "ffmpeg/AvOutputFileContext.hpp"
#include "buffers/EncodedVideoBuffer.hpp"
#include "input/VideoStreamReader.hpp"

#include <opencv2/core/cuda.hpp>

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
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

std::atomic<bool> capture_active{true};

void handleTerminationSignal(int) {
    ::capture_active = false;
}

int main(int argument_count, char** argument_values) {
    const bool help_requested = argument_count == 2 &&
        (std::string(argument_values[1]) == "--help" || std::string(argument_values[1]) == "-h");
    if (help_requested || argument_count > 4) {
        std::cout << "Uso: motion_mog2 [fonte] [diretorio_saida] [pacotes_por_trecho]\n"
                     "Padrao: rtsp://127.0.0.1:8554/video ./recordings 1000\n"
                     "O corte ocorre no proximo keyframe apos atingir o alvo de pacotes.\n";
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

        if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
            std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
            return 1;
        }
        cv::cuda::setDevice(0);

        VideoStreamReader video_reader(input_source);
        const auto video_stream_parameters = video_reader.getVideoStreamParameters();
        if (!video_stream_parameters) {
            throw std::invalid_argument("Os parametros da stream devem estar disponiveis apos construir o reader.");
        }
        EncodedVideoBuffer encoded_video_buffer(target_recording_packet_count);
        std::filesystem::create_directories(output_directory);

        std::cout << "STREAM_FPS fps=" << video_reader.getFramesPerSecond() << "\n";
        std::cout << "STREAM_RESOLUTION width=" << video_reader.getWidth()
                  << " height=" << video_reader.getHeight() << "\n";
        std::cout << "MP4_RECORDING target_encoded_packets=" << target_recording_packet_count
                  << " directory=" << output_directory << "\n";
        std::cout.flush();

        cv::cuda::Stream cuda_stream;
        DecodedFrame decoded_frame;
        std::vector<AvPacketPtr> pending_encoded_packets;

        AvOutputFileContextPtr output_io_context;
        AvMp4OutputContextPtr mp4_output_context;
        std::filesystem::path current_output_file_path;
        std::size_t recording_read_count = 0;
        std::size_t recording_encoded_packet_count = 0;
        std::size_t skipped_initial_packet_count = 0;
        std::size_t completed_recording_count = 0;
        bool next_keyframe_boundary_pending = false;

        const auto encoded_packet_writer = [&](std::vector<AvPacketPtr>&& encoded_packets_for_output) {
            writeMp4Packets(*mp4_output_context, std::move(encoded_packets_for_output), video_stream_parameters);
        };
        const auto recording_finalizer = [&]() {
            writeMp4Packets(*mp4_output_context, encoded_video_buffer.takeBufferedEncodedPackets(), video_stream_parameters);
            writeMp4Trailer(*mp4_output_context);
            mp4_output_context.reset();
            output_io_context.reset();
            ++completed_recording_count;
            std::cout << "MP4_SAVED path=" << current_output_file_path
                      << " decoded_reads=" << recording_read_count
                      << " encoded_packets=" << recording_encoded_packet_count << "\n";
            std::cout.flush();
        };

        while (::capture_active || !pending_encoded_packets.empty()) {
            const bool new_decoded_frame_required = pending_encoded_packets.empty();
            if (new_decoded_frame_required) {
                if (!video_reader.read(decoded_frame, pending_encoded_packets, cuda_stream)) {
                    break;
                }
                cuda_stream.waitForCompletion();
            }

            for (const auto& packet : pending_encoded_packets) {
                if (!packet) {
                    throw std::invalid_argument("A leitura retornou um AVPacket nulo.");
                }
            }

            if (!mp4_output_context || next_keyframe_boundary_pending) {
                auto next_keyframe = std::find_if(
                    pending_encoded_packets.begin(),
                    pending_encoded_packets.end(),
                    [](const AvPacketPtr& packet) { return (packet->flags & AV_PKT_FLAG_KEY) != 0; }
                );
                if (!mp4_output_context && next_keyframe == pending_encoded_packets.end()) {
                    skipped_initial_packet_count += pending_encoded_packets.size();
                    pending_encoded_packets.clear();
                    continue;
                }
                if (next_keyframe != pending_encoded_packets.end()) {
                    if (mp4_output_context) {
                        std::vector<AvPacketPtr> packets_before_keyframe;
                        for (auto packet = pending_encoded_packets.begin(); packet != next_keyframe; ++packet) {
                            packets_before_keyframe.push_back(std::move(*packet));
                        }
                        recording_encoded_packet_count += packets_before_keyframe.size();
                        encoded_packet_writer(std::move(packets_before_keyframe));
                        recording_finalizer();
                    } else {
                        skipped_initial_packet_count += static_cast<std::size_t>(
                            next_keyframe - pending_encoded_packets.begin()
                        );
                    }
                    pending_encoded_packets.erase(
                        pending_encoded_packets.begin(), next_keyframe
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
                recording_read_count = 0;
                recording_encoded_packet_count = 0;
                next_keyframe_boundary_pending = false;
                std::cout << "MP4_STARTED path=" << current_output_file_path << "\n";
                std::cout.flush();
            }

            if (new_decoded_frame_required) {
                ++recording_read_count;
            }
            if (next_keyframe_boundary_pending) {
                recording_encoded_packet_count += pending_encoded_packets.size();
                encoded_packet_writer(std::move(pending_encoded_packets));
                pending_encoded_packets.clear();
            } else {
                const std::size_t pending_packet_count = pending_encoded_packets.size();
                encoded_video_buffer.appendPackets(pending_encoded_packets);
                recording_encoded_packet_count += pending_packet_count - pending_encoded_packets.size();
                if (encoded_video_buffer.checkIfFull()) {
                    writeMp4Packets(*mp4_output_context, encoded_video_buffer.takeBufferedEncodedPackets(), video_stream_parameters);
                    next_keyframe_boundary_pending = true;
                }
            }
        }

        if (mp4_output_context) {
            recording_finalizer();
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
