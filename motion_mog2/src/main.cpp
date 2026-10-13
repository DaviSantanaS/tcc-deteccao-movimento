#include "AvOutputFormatContext.hpp"
#include "AvOutputIoContext.hpp"
#include "EncodedVideoBuffer.hpp"
#include "EncodedVideoPackets.hpp"
#include "Mog2MotionDetector.hpp"
#include "PreEventConfig.hpp"
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
#include <thread>
#include <utility>
#include <vector>

std::atomic<bool> keep_running{true};

void handle_signal(int) {
    keep_running = false;
}

int main(int argc, char** argv) {
    const bool show_help = argc == 2 &&
        (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h");
    /*
    if (show_help || argc > 6) {
        std::cout << "Uso: motion_mog2 [rtsp_url] [motion_threshold_percent] "
                     "[motion_start_frames] [motion_end_frames] [pre_event_seconds]\n";
        return show_help ? 0 : 1;
    }
    */
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
        /*
        const double motion_threshold_percent =
            (argc > 2) ? std::stod(argv[2]) : 1.0;
        const int motion_start_frames =
            (argc > 3) ? std::stoi(argv[3]) : 2;
        const int motion_end_frames =
            (argc > 4) ? std::stoi(argv[4]) : 3;
        const double mog2_learning_rate = 0.01;
        const double pre_event_seconds =
            (argc > 5) ? parsePreEventSeconds(argv[5]) : 0.0;
        */
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

        // Responsável por RTSP, NVDEC, FPS/resolução e pacotes codificados.
        VideoStreamReader video_reader(input_source);
        /*
        const uint64_t pre_event_frame_count =
            calculatePreEventFrameCount(pre_event_seconds, video_reader.fps());

        // Responsável por MOG2, percentual de foreground, warm-up e debounce.
        Mog2MotionDetector motion_detector(
            video_reader.width(),
            video_reader.height(),
            video_reader.fps(),
            motion_threshold_percent,
            motion_start_frames,
            motion_end_frames,
            mog2_learning_rate
        );

        // Responsável por acompanhar o GOP e montar o buffer codificado do movimento.
        EncodedVideoBuffer encoded_video_buffer(pre_event_frame_count);
        */
        EncodedVideoBuffer encoded_video_buffer(target_decoded_frame_read_count);
        std::filesystem::create_directories(output_directory);

        std::cout << "STREAM_FPS fps=" << video_reader.fps() << "\n";
        std::cout << "STREAM_RESOLUTION width=" << video_reader.width()
                  << " height=" << video_reader.height() << "\n";
        /*
        std::cout << "MOTION_THRESHOLD percent="
                  << motion_threshold_percent << "\n";
        std::cout << "MOG2_WARMUP frames="
                  << motion_detector.warmupFrameCount() << "\n";
        std::cout << "PRE_EVENT seconds=" << pre_event_seconds
                  << " frames=" << pre_event_frame_count
                  << " reference=decoded_frame_index\n";
        std::cout << "MOTION_BUFFER mode="
                  << (pre_event_frame_count > 0
                      ? "pre_event_key_frame_to_motion_off"
                      : "previous_key_frame_to_motion_off") << "\n";
        */
        std::cout << "MP4_RECORDING target_decoded_reads=" << target_decoded_frame_read_count
                  << " directory=" << output_directory << "\n";
        std::cout.flush();

        cv::cuda::Stream cuda_stream;
        DecodedFrame decoded_frame;
        EncodedVideoPackets encoded_video_packets;

        // O contexto MP4 deve ser destruido antes do I/O que ele referencia.
        AvOutputIoContextPtr output_io_context;
        AvOutputFormatContextPtr mp4_output_context;
        std::shared_ptr<const VideoStreamParameters> recording_stream_parameters;
        std::filesystem::path current_output_file_path;
        std::size_t recording_decoded_read_count = 0;
        std::size_t recording_encoded_packet_count = 0;
        std::size_t skipped_initial_packet_count = 0;
        std::size_t completed_recording_count = 0;
        bool waiting_for_next_keyframe = false;

        const auto write_packets_from_read = [&](EncodedVideoPackets&& packets_from_read) {
            std::vector<EncodedVideoPackets> packets_to_write;
            packets_to_write.push_back(std::move(packets_from_read));
            writeMp4Packets(*mp4_output_context, std::move(packets_to_write));
        };
        const auto finish_recording = [&]() {
            writeMp4Packets(*mp4_output_context, encoded_video_buffer.takeBufferedReadPackets());
            writeMp4Trailer(*mp4_output_context);
            mp4_output_context.reset();
            output_io_context.reset();
            recording_stream_parameters.reset();
            ++completed_recording_count;
            std::cout << "MP4_SAVED path=" << current_output_file_path
                      << " decoded_reads=" << recording_decoded_read_count
                      << " encoded_packets=" << recording_encoded_packet_count << "\n";
            std::cout.flush();
        };

        while (keep_running) {
            /*
            bool decoded_frame_ready = false;

            try {
                decoded_frame_ready = video_reader.read(
                    decoded_frame,
                    encoded_video_packets,
                    cuda_stream
                );
            } catch (const cv::Exception& error) {
                std::cerr << "[fatal] NVDEC exception: " << error.what() << "\n";
                break;
            } catch (const std::exception& error) {
                std::cerr << "[fatal] " << error.what() << "\n";
                break;
            }

            if (!decoded_frame_ready) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            */
            if (!video_reader.read(decoded_frame, encoded_video_packets, cuda_stream)) {
                break;
            }
            cuda_stream.waitForCompletion();

            /*
            encoded_video_buffer.updatePreEventBuffer(
                encoded_video_packets.encoded_packets,
                decoded_frame.decoded_frame_index
            );

            const MotionState motion_state = motion_detector.process(
                decoded_frame.decoded_frame_gpu,
                decoded_frame.decoded_frame_index,
                cuda_stream
            );

            if (motion_state.started) {
                const MotionBufferStartInfo start_info =
                    encoded_video_buffer.startMotion(
                        decoded_frame.decoded_frame_index,
                        encoded_video_packets.encoded_packets
                    );

                std::cout << "MOTION_ON frame="
                          << decoded_frame.decoded_frame_index << "\n";
                std::cout << "MOTION_BUFFER_START motion_frame="
                          << start_info.motion_decoded_frame_index
                          << " start_frame="
                          << start_info.start_decoded_frame_index
                          << " extra_frames_before_motion="
                          << start_info.extra_decoded_frames_before_motion
                          << " gop_packets="
                          << start_info.gop_encoded_packet_count
                          << " starts_with_key_frame="
                          << (start_info.starts_with_key_frame ? 1 : 0)
                          << " requested_pre_event_frames="
                          << start_info.requested_pre_event_frame_count
                          << " pre_event_history_sufficient="
                          << (start_info.pre_event_history_sufficient ? 1 : 0)
                          << "\n";
                std::cout.flush();
            } else if (motion_state.active) {
                // No frame do MOTION_ON o GOP já contém os pacotes atuais;
                // por isso só anexamos diretamente nos frames seguintes.
                encoded_video_buffer.appendMotionPackets(
                    encoded_video_packets.encoded_packets
                );
            }

            if (motion_state.ended) {
                const MotionBufferCompleteInfo complete_info =
                    encoded_video_buffer.finishMotion();

                std::cout << "MOTION_OFF frame="
                          << decoded_frame.decoded_frame_index << "\n";
                std::cout << "MOTION_BUFFER_COMPLETE packets="
                          << complete_info.encoded_packet_count
                          << " bytes=" << complete_info.encoded_byte_count
                          << " duration_seconds="
                          << complete_info.duration_seconds
                          << " key_frames=" << complete_info.key_frame_count
                          << " starts_with_key_frame="
                          << (complete_info.starts_with_key_frame ? 1 : 0)
                          << " extra_frames_before_motion="
                          << complete_info.extra_decoded_frames_before_motion
                          << "\n";
                std::cout.flush();
            }
            */

            for (const auto& packet : encoded_video_packets.encoded_packets) {
                if (!packet) {
                    throw std::invalid_argument("A leitura retornou um AVPacket nulo.");
                }
            }
            if (mp4_output_context &&
                encoded_video_packets.video_stream_parameters.get() != recording_stream_parameters.get()) {
                throw std::runtime_error("A configuracao da stream mudou durante a gravacao.");
            }

            if (!mp4_output_context || waiting_for_next_keyframe) {
                auto next_keyframe = std::find_if(
                    encoded_video_packets.encoded_packets.begin(),
                    encoded_video_packets.encoded_packets.end(),
                    [](const AvPacketPtr& packet) { return (packet->flags & AV_PKT_FLAG_KEY) != 0; }
                );
                if (!mp4_output_context && next_keyframe == encoded_video_packets.encoded_packets.end()) {
                    skipped_initial_packet_count += encoded_video_packets.encoded_packets.size();
                    continue;
                }
                if (next_keyframe != encoded_video_packets.encoded_packets.end()) {
                    if (mp4_output_context) {
                        EncodedVideoPackets packets_before_keyframe{{}, recording_stream_parameters};
                        for (auto packet = encoded_video_packets.encoded_packets.begin(); packet != next_keyframe; ++packet) {
                            packets_before_keyframe.encoded_packets.push_back(std::move(*packet));
                        }
                        recording_encoded_packet_count += packets_before_keyframe.encoded_packets.size();
                        write_packets_from_read(std::move(packets_before_keyframe));
                        finish_recording();
                    } else {
                        skipped_initial_packet_count += static_cast<std::size_t>(
                            next_keyframe - encoded_video_packets.encoded_packets.begin()
                        );
                    }
                    encoded_video_packets.encoded_packets.erase(
                        encoded_video_packets.encoded_packets.begin(), next_keyframe
                    );
                }
            }

            if (!mp4_output_context) {
                recording_stream_parameters = encoded_video_packets.video_stream_parameters;
                if (!recording_stream_parameters) {
                    throw std::invalid_argument("Os parametros da stream devem acompanhar os pacotes.");
                }
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
                (void)createMp4VideoStream(*mp4_output_context, *recording_stream_parameters);
                output_io_context = openOutputFile(current_output_file_path);
                writeMp4Header(*mp4_output_context, output_io_context);
                recording_decoded_read_count = 0;
                recording_encoded_packet_count = 0;
                waiting_for_next_keyframe = false;
                std::cout << "MP4_STARTED path=" << current_output_file_path << "\n";
                std::cout.flush();
            }

            ++recording_decoded_read_count;
            recording_encoded_packet_count += encoded_video_packets.encoded_packets.size();
            if (waiting_for_next_keyframe) {
                write_packets_from_read(std::move(encoded_video_packets));
            } else if (encoded_video_buffer.appendReadPackets(std::move(encoded_video_packets))) {
                writeMp4Packets(*mp4_output_context, encoded_video_buffer.takeBufferedReadPackets());
                waiting_for_next_keyframe = true;
            }
        }

        /*
        if (motion_detector.isMotionActive()) {
            const MotionBufferCompleteInfo current_info =
                encoded_video_buffer.currentMotionInfo();

            std::cout << "MOTION_OFF frame="
                      << video_reader.processedFrameCount() << "\n";
            std::cout << "MOTION_BUFFER_ON_SHUTDOWN packets="
                      << current_info.encoded_packet_count
                      << " bytes=" << current_info.encoded_byte_count
                      << " starts_with_key_frame="
                      << (current_info.starts_with_key_frame ? 1 : 0)
                      << " extra_frames_before_motion="
                      << current_info.extra_decoded_frames_before_motion
                      << "\n";
            std::cout.flush();
        }
        */
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
