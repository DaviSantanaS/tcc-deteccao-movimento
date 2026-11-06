#include "ffmpeg/AvMp4OutputContext.hpp"
#include "ffmpeg/AvOutputFileContext.hpp"
#include "buffers/EncodedPacketQueue.hpp"
#include "config/EventRecordingConfig.hpp"
#include "config/PreEventConfig.hpp"
#include "detectors/Mog2MotionDetector.hpp"
#include "input/VideoStreamReader.hpp"

#include <opencv2/core/cuda.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
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

namespace {

enum class RecordingState {
    idle,
    waiting_keyframe,
    recording,
    post_event,
    closing_at_keyframe
};

std::filesystem::path buildRecordingFilePath(const std::filesystem::path& output_directory) {
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
    std::filesystem::path recording_file_path = output_directory / (recording_file_stem + ".mp4");
    for (std::size_t suffix = 1;
         std::filesystem::symlink_status(recording_file_path).type() != std::filesystem::file_type::not_found;
         ++suffix) {
        std::ostringstream unique_file_name;
        unique_file_name << recording_file_stem << '_' << std::setfill('0') << std::setw(3)
                         << suffix << ".mp4";
        recording_file_path = output_directory / unique_file_name.str();
    }

    return recording_file_path;
}

}

// Compartilhada entre o handler de sinal e o laco de captura para encerrar a leitura.
std::atomic<bool> capture_active{true};

void handleTerminationSignal(int) {
    ::capture_active = false;
}

int main(int argument_count, char** argument_values) {
    const bool help_requested = argument_count == 2 &&
        (std::string(argument_values[1]) == "--help" || std::string(argument_values[1]) == "-h");
    if (help_requested || argument_count > 9) {
        std::cout << "Uso: motion_mog2 [fonte] [diretorio_saida] [pre_evento_s] [limiar_%] [inicio_frames] [fim_frames] [pos_evento_s] [rtsp_transporte]\n"
                     "Padrao: rtsp://127.0.0.1:8554/video ./recordings 5 1.0 2 3 5 tcp\n"
                     "rtsp_transporte: tcp, udp ou auto; ignorado para arquivo local.\n"
                     "A gravacao por evento comeca em keyframe e inclui o pre-evento disponivel.\n";
        return help_requested ? 0 : 1;
    }

    signal(SIGINT, handleTerminationSignal);
    signal(SIGTERM, handleTerminationSignal);

    try {
        const EventRecordingConfig recording_config = parseEventRecordingArguments(argument_count, argument_values);
        // Valor negativo seleciona o aprendizado automatico do OpenCV.
        constexpr double learning_rate = -1.0;

        if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
            std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
            return 1;
        }
        cv::cuda::setDevice(0);

        if (recording_config.input_source.rfind("rtsp://", 0) == 0) {
            std::cout << "RTSP_TRANSPORT value=" << recording_config.rtsp_transport << std::endl;
        }
        VideoStreamReader video_reader(recording_config.input_source, recording_config.rtsp_transport);
        Mog2MotionDetector motion_detector(
            video_reader.getWidth(),
            video_reader.getHeight(),
            video_reader.getFramesPerSecond(),
            recording_config.motion_threshold_percent,
            recording_config.motion_start_frame_count,
            recording_config.motion_end_frame_count,
            learning_rate
        );
        const double stream_fps = video_reader.getFramesPerSecond();
        const auto calculate_packet_count = [stream_fps](double seconds) {
            std::uint64_t frame_count = 0;
            try {
                frame_count = calculatePreEventFrameCount(seconds, stream_fps);
            } catch (const std::runtime_error& error) {
                throw std::invalid_argument(error.what());
            }
            if (frame_count > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("Contagem de pacotes excede o limite suportado.");
            }
            return static_cast<std::size_t>(frame_count);
        };
        const auto add_packet_counts = [](std::size_t first_count, std::size_t second_count) {
            if (second_count > std::numeric_limits<std::size_t>::max() - first_count) {
                throw std::invalid_argument("Soma de pacotes excede o limite suportado.");
            }
            return first_count + second_count;
        };
        // OpenCV 4.10: FrameQueue::dequeue retira todos os pacotes raw;
        // VideoParser::HandleVideoSequence limita as superficies de decodificacao a 32.
        constexpr std::size_t maximum_decoder_lookahead_frame_count = 32;
        // Suposicao a ajustar depois de medir o GOP da camera.
        constexpr double maximum_expected_gop_seconds = 20.0;
        // av_read_frame (libavformat/avformat.h) entrega um frame codificado
        // por pacote de video; a janela em frames pode ser contada em pacotes.
        const std::size_t pre_event_packet_count = calculate_packet_count(recording_config.pre_event_seconds);
        const std::size_t post_event_packet_count = calculate_packet_count(recording_config.post_event_seconds);
        const std::size_t start_margin_packet_count = add_packet_counts(
            maximum_decoder_lookahead_frame_count,
            static_cast<std::size_t>(recording_config.motion_start_frame_count - 1)
        );
        // A janela e ancorada na fronteira contabilizada: pacotes sem frame
        // exibido deslocariam o indice do decoder. A margem cobre o lookahead
        // e os frames usados para confirmar o inicio do movimento.
        const std::size_t lookback_packet_count = add_packet_counts(
            pre_event_packet_count, start_margin_packet_count
        );
        const std::size_t maximum_idle_packet_count = add_packet_counts(
            lookback_packet_count, calculate_packet_count(maximum_expected_gop_seconds)
        );
        const auto video_stream_parameters = video_reader.getVideoStreamParameters();
        if (!video_stream_parameters) {
            throw std::invalid_argument("Os parametros da stream devem estar disponiveis apos construir o reader.");
        }
        std::filesystem::create_directories(recording_config.output_directory);

        std::cout << "STREAM_FPS fps=" << video_reader.getFramesPerSecond() << "\n";
        std::cout << "STREAM_RESOLUTION width=" << video_reader.getWidth()
                  << " height=" << video_reader.getHeight() << "\n";
        std::cout << "PRE_EVENT seconds=" << recording_config.pre_event_seconds << " packets=" << pre_event_packet_count
                  << " margin_packets=" << start_margin_packet_count
                  << " idle_cap_packets=" << maximum_idle_packet_count << "\n";
        std::cout << "POST_EVENT seconds=" << recording_config.post_event_seconds
                  << " packets=" << post_event_packet_count << "\n";
        std::cout << "MOG2 warmup_frames=" << motion_detector.getWarmupFrameCount() << "\n";
        std::cout.flush();

        cv::cuda::Stream cuda_stream;
        DecodedFrame decoded_frame;

        AvOutputFileContextPtr output_io_context;
        AvMp4OutputContextPtr mp4_output_context;
        std::filesystem::path current_output_file_path;
        std::size_t recording_read_count = 0;
        std::size_t recording_encoded_packet_count = 0;
        std::size_t completed_recording_count = 0;
        RecordingState recording_state = RecordingState::idle;
        std::size_t post_remaining_packet_count = 0;

        const auto recording_opener = [&]() {
            current_output_file_path = buildRecordingFilePath(recording_config.output_directory);
            mp4_output_context = allocateMp4OutputContext();
            mp4_output_context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
            (void)createMp4VideoStream(*mp4_output_context, *video_stream_parameters);
            output_io_context = openOutputFile(current_output_file_path);
            writeMp4Header(*mp4_output_context, output_io_context);
            recording_read_count = 1;
            recording_encoded_packet_count = 0;
            std::cout << "MP4_STARTED path=" << current_output_file_path << std::endl;
        };
        const auto encoded_packet_writer = [&](std::vector<AvPacketPtr>&& encoded_packets_for_output) {
            if (encoded_packets_for_output.empty()) {
                return;
            }
            if (recording_encoded_packet_count == 0 &&
                (!encoded_packets_for_output.front() ||
                 (encoded_packets_for_output.front()->flags & AV_PKT_FLAG_KEY) == 0)) {
                throw std::logic_error("O primeiro lote do MP4 deve comecar em keyframe.");
            }
            const std::size_t packet_count = encoded_packets_for_output.size();
            writeMp4Packets(*mp4_output_context, std::move(encoded_packets_for_output), video_stream_parameters);
            recording_encoded_packet_count += packet_count;
        };
        const auto recording_finalizer = [&]() {
            writeMp4Trailer(*mp4_output_context);
            mp4_output_context.reset();
            output_io_context.reset();
            ++completed_recording_count;
            std::cout << "MP4_SAVED path=" << current_output_file_path
                      << " decoded_reads=" << recording_read_count
                      << " encoded_packets=" << recording_encoded_packet_count << std::endl;
        };

        // A escrita fica na thread do read(); com allowFrameDrop=false,
        // o I/O pode atrasar a leitura RTSP. Uma thread de escrita fica para depois.
        while (::capture_active) {
            if (!video_reader.read(decoded_frame, cuda_stream)) {
                break;
            }
            cuda_stream.waitForCompletion();
            const MotionState motion_state = motion_detector.detectMotion(
                decoded_frame.decoded_frame_gpu,
                decoded_frame.decoded_frame_index,
                cuda_stream
            );
            if (motion_state.started) {
                std::cout << "MOTION_STARTED frame=" << decoded_frame.decoded_frame_index << std::endl;
            }
            if (motion_state.ended) {
                std::cout << "MOTION_ENDED frame=" << decoded_frame.decoded_frame_index << std::endl;
            }
            EncodedPacketQueue& encoded_packet_queue = video_reader.getEncodedPacketQueue();
            if (mp4_output_context) {
                ++recording_read_count;
            }

            // take* retorna o lote por valor; o lock da fila ja foi liberado
            // quando comeca a escrita no arquivo.
            switch (recording_state) {
                case RecordingState::idle: {
                    if (motion_state.started) {
                        std::cout << "EVENT_STARTED frame=" << decoded_frame.decoded_frame_index << std::endl;
                        auto encoded_packets = encoded_packet_queue.takeCountedPacketsFromLookback(lookback_packet_count);
                        if (encoded_packets.empty()) {
                            recording_state = RecordingState::waiting_keyframe;
                            std::cout << "EVENT_WAITING_KEYFRAME" << std::endl;
                        } else {
                            const std::size_t pre_event_written_packet_count = encoded_packets.size();
                            recording_opener();
                            encoded_packet_writer(std::move(encoded_packets));
                            std::cout << "PRE_EVENT_WRITTEN packets=" << pre_event_written_packet_count << std::endl;
                            recording_state = RecordingState::recording;
                        }
                    } else {
                        (void)encoded_packet_queue.trimCountedPackets(lookback_packet_count, maximum_idle_packet_count);
                    }
                    break;
                }
                case RecordingState::waiting_keyframe: {
                    (void)encoded_packet_queue.discardCountedPacketsBeforeFirstKeyframe();
                    auto encoded_packets = encoded_packet_queue.takeCountedPackets();
                    if (!encoded_packets.empty()) {
                        recording_opener();
                        encoded_packet_writer(std::move(encoded_packets));
                        recording_state = RecordingState::recording;
                    } else if (motion_state.ended) {
                        std::cout << "EVENT_DISCARDED_NO_KEYFRAME" << std::endl;
                        recording_state = RecordingState::idle;
                    }
                    break;
                }
                case RecordingState::recording:
                    encoded_packet_writer(encoded_packet_queue.takeCountedPackets());
                    break;
                case RecordingState::post_event: {
                    auto encoded_packets = encoded_packet_queue.takeCountedPackets();
                    if (motion_state.started) {
                        std::cout << "EVENT_RESUMED frame=" << decoded_frame.decoded_frame_index << std::endl;
                        encoded_packet_writer(std::move(encoded_packets));
                        recording_state = RecordingState::recording;
                    } else {
                        const std::size_t written_packet_count = encoded_packets.size();
                        encoded_packet_writer(std::move(encoded_packets));
                        post_remaining_packet_count -= std::min(post_remaining_packet_count, written_packet_count);
                        if (post_remaining_packet_count == 0) {
                            std::cout << "POST_EVENT_EXPIRED frame=" << decoded_frame.decoded_frame_index << std::endl;
                            recording_state = RecordingState::closing_at_keyframe;
                        }
                    }
                    break;
                }
                case RecordingState::closing_at_keyframe: {
                    if (motion_state.started) {
                        std::cout << "EVENT_RESUMED frame=" << decoded_frame.decoded_frame_index << std::endl;
                        encoded_packet_writer(encoded_packet_queue.takeCountedPackets());
                        recording_state = RecordingState::recording;
                    } else {
                        bool keyframe_found = false;
                        encoded_packet_writer(
                            encoded_packet_queue.takeCountedPacketsBeforeNextKeyframe(keyframe_found)
                        );
                        if (keyframe_found) {
                            recording_finalizer();
                            recording_state = RecordingState::idle;
                        }
                    }
                    break;
                }
            }

            // Tambem cobre ended no mesmo read que abriu um arquivo apos esperar keyframe.
            if (recording_state == RecordingState::recording && motion_state.ended) {
                std::cout << "EVENT_ENDED frame=" << decoded_frame.decoded_frame_index << std::endl;
                post_remaining_packet_count = post_event_packet_count;
                recording_state = post_remaining_packet_count == 0
                    ? RecordingState::closing_at_keyframe
                    : RecordingState::post_event;
            }
        }

        // Provisorio ate a spec 06: os pacotes ainda nao contabilizados
        // permanecem na fila; aqui so terminamos o arquivo que ja esta aberto.
        if (mp4_output_context) {
            encoded_packet_writer(video_reader.getEncodedPacketQueue().takeCountedPackets());
            recording_finalizer();
        }
        std::cout << "CAPTURE_COMPLETE decoded_reads=" << video_reader.getDecodedFrameCount()
                  << " saved_files=" << completed_recording_count << "\n";
    } catch (const cv::Exception& error) {
        std::cerr << "[fatal] CUDA/NVDEC: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << "\n";
        return 2;
    }

    return 0;
}
