#include "EncodedVideoBuffer.hpp"
#include "Mog2MotionDetector.hpp"
#include "VideoStreamReader.hpp"

#include <opencv2/core/cuda.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

std::atomic<bool> keep_running{true};

void handle_signal(int) {
    keep_running = false;
}

int main(int argc, char** argv) {
    // args: <rtsp_url> [motion_threshold_percent] [motion_start_frames] [motion_end_frames]
    const std::string rtsp_url =
        (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    const double motion_threshold_percent =
        (argc > 2) ? std::stod(argv[2]) : 1.0;
    const int motion_start_frames =
        (argc > 3) ? std::stoi(argv[3]) : 2;
    const int motion_end_frames =
        (argc > 4) ? std::stoi(argv[4]) : 3;
    const double mog2_learning_rate = 0.01;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
        return 1;
    }
    cv::cuda::setDevice(0);

    try {
        // Responsável por RTSP, NVDEC, FPS/resolução e pacotes codificados.
        VideoStreamReader video_reader(rtsp_url);

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
        EncodedVideoBuffer encoded_video_buffer;

        std::cout << "STREAM_FPS fps=" << video_reader.fps() << "\n";
        std::cout << "STREAM_RESOLUTION width=" << video_reader.width()
                  << " height=" << video_reader.height() << "\n";
        std::cout << "MOTION_THRESHOLD percent="
                  << motion_threshold_percent << "\n";
        std::cout << "MOG2_WARMUP frames="
                  << motion_detector.warmupFrameCount() << "\n";
        std::cout << "MOTION_BUFFER mode=previous_key_frame_to_motion_off\n";
        std::cout.flush();

        cv::cuda::Stream cuda_stream;
        VideoFrameData video_frame_data;

        while (keep_running) {
            bool decoded_frame_ready = false;

            try {
                decoded_frame_ready = video_reader.read(video_frame_data, cuda_stream);
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

            // Mantém, em paralelo à detecção, o trecho codificado desde o
            // keyframe mais recente até o frame atual.
            encoded_video_buffer.updateCurrentGop(
                video_frame_data.encoded_packets,
                video_frame_data.decoded_frame_index
            );

            const MotionState motion_state = motion_detector.process(
                video_frame_data.decoded_frame_gpu,
                video_frame_data.decoded_frame_index,
                cuda_stream
            );

            if (motion_state.started) {
                const MotionBufferStartInfo start_info =
                    encoded_video_buffer.startMotion(
                        video_frame_data.decoded_frame_index,
                        video_frame_data.encoded_packets
                    );

                std::cout << "MOTION_ON frame="
                          << video_frame_data.decoded_frame_index << "\n";
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
                          << "\n";
                std::cout.flush();
            } else if (motion_state.active) {
                // No frame do MOTION_ON o GOP já contém os pacotes atuais;
                // por isso só anexamos diretamente nos frames seguintes.
                encoded_video_buffer.appendMotionPackets(
                    video_frame_data.encoded_packets
                );
            }

            if (motion_state.ended) {
                const MotionBufferCompleteInfo complete_info =
                    encoded_video_buffer.finishMotion();

                std::cout << "MOTION_OFF frame="
                          << video_frame_data.decoded_frame_index << "\n";
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
        }

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
    } catch (const cv::Exception& error) {
        std::cerr << "[fatal] Falha ao abrir NVDEC: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "[fatal] " << error.what() << "\n";
        return 2;
    }

    return 0;
}
