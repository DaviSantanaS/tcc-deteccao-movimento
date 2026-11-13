#include "app/CaptureEnd.hpp"
#include "config/EventRecordingConfig.hpp"
#include "config/PreEventConfig.hpp"
#include "detectors/Mog2MotionDetector.hpp"
#include "input/VideoStreamReader.hpp"
#include "recording/EventRecorder.hpp"
#include "recording/Mp4EventPacketSink.hpp"

#include <opencv2/core/cuda.hpp>

extern "C" {
#include <libavutil/error.h>
}

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

static_assert(std::atomic<bool>::is_always_lock_free, "O handler de sinal exige atomic<bool> lock-free.");
// Compartilhada pelo handler, laco e callback FFmpeg. Escrita em atomic<bool>
// lock-free e segura no handler de sinal; o callback interrompe a leitura bloqueada.
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

    CaptureEndReason capture_end_reason = CaptureEndReason::read_error;
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
        VideoStreamReader video_reader(recording_config.input_source, recording_config.rtsp_transport, &::capture_active);
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

        Mp4EventPacketSink packet_sink(recording_config.output_directory, video_stream_parameters);
        EventRecorder event_recorder(
            video_reader.getEncodedPacketQueue(), packet_sink,
            EventRecorderConfig{lookback_packet_count, maximum_idle_packet_count, post_event_packet_count},
            std::cout
        );

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
            event_recorder.processDecodedFrame(motion_state, decoded_frame.decoded_frame_index);
        }

        video_reader.stopDecoding();
        const PacketReadEnd packet_read_end = video_reader.getPacketReadEnd();
        const bool interrupted_by_signal = !::capture_active;
        std::error_code file_status_error;
        const bool source_is_live = !std::filesystem::is_regular_file(recording_config.input_source, file_status_error);
        capture_end_reason = classifyCaptureEnd(packet_read_end, source_is_live, interrupted_by_signal);
        char error_detail[AV_ERROR_MAX_STRING_SIZE] = {};
        if (packet_read_end.ffmpeg_error_code != 0) {
            av_strerror(packet_read_end.ffmpeg_error_code, error_detail, sizeof(error_detail));
        }
        std::cout << "CAPTURE_END reason=" << getCaptureEndReasonName(capture_end_reason)
                  << " av_error=" << packet_read_end.ffmpeg_error_code
                  << " detail=\"" << error_detail << "\"" << std::endl;
        auto remaining_encoded_packets = video_reader.getEncodedPacketQueue().takeAllPackets();
        event_recorder.finishCapture(std::move(remaining_encoded_packets));
        std::cout << "CAPTURE_COMPLETE decoded_reads=" << video_reader.getDecodedFrameCount()
                  << " saved_files=" << event_recorder.getCompletedRecordingCount() << "\n";
    } catch (const cv::Exception& error) {
        std::cerr << "[fatal] CUDA/NVDEC: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << "\n";
        return 2;
    }

    return getExitCodeForCaptureEnd(capture_end_reason);
}
