#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>

#include <csignal>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <algorithm>
#include <utility>
#include <iostream>

struct EncodedPacket {
    std::vector<unsigned char> data;
    std::chrono::steady_clock::time_point received_at;
    bool has_key_frame = false;
};

std::atomic<bool> keep_running{true};
void handle_signal(int){ keep_running = false; }

int main(int argc, char** argv) {
    // args: <rtsp_url> [motion_threshold_percent] [motion_start_frames] [motion_end_frames]
    //       [pre_event_seconds] [post_event_seconds]
    std::string rtsp_url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double motion_threshold_percent = (argc > 2) ? std::stod(argv[2]) : 1.0;
    int motion_start_frames = (argc > 3) ? std::stoi(argv[3]) : 2;
    int motion_end_frames   = (argc > 4) ? std::stoi(argv[4]) : 3;
    double pre_event_seconds = (argc > 5) ? std::stod(argv[5]) : 5.0;
    double post_event_seconds = (argc > 6) ? std::stod(argv[6]) : 5.0;
    const double mog2_learning_rate = 0.01;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
        return 1;
    }
    cv::cuda::setDevice(0);

    if (pre_event_seconds <= 0.0) {
        std::cerr << "[fatal] pre_event_seconds deve ser maior que zero. Valor: "
                  << pre_event_seconds << "\n";
        return 2;
    }

    if (post_event_seconds <= 0.0) {
        std::cerr << "[fatal] post_event_seconds deve ser maior que zero. Valor: "
                  << post_event_seconds << "\n";
        return 3;
    }

    cv::cudacodec::VideoReaderInitParams reader_params;
    reader_params.allowFrameDrop = true;
    reader_params.rawMode = true;
    reader_params.udpSource = true;

    std::vector<int> source_params;
    cv::Ptr<cv::cudacodec::VideoReader> video_reader;
    try {
        video_reader = cv::cudacodec::createVideoReader(rtsp_url, source_params, reader_params);
    } catch (const cv::Exception& error) {
        std::cerr << "[fatal] Falha ao abrir NVDEC: " << error.what() << "\n";
        return 4;
    }

    const cv::cudacodec::FormatInfo stream_format = video_reader->format();
    const double stream_fps = stream_format.fps;
    const int frame_width = stream_format.width;
    const int frame_height = stream_format.height;

    if (stream_fps <= 0.0) {
        std::cerr << "[fatal] FPS invalido informado pelo stream: " << stream_fps << "\n";
        return 5;
    }

    if (frame_width <= 0 || frame_height <= 0) {
        std::cerr << "[fatal] Resolucao invalida informada pelo stream: "
                  << frame_width << "x" << frame_height << "\n";
        return 6;
    }

    if (motion_threshold_percent <= 0.0 || motion_threshold_percent > 100.0) {
        std::cerr << "[fatal] motion_threshold_percent deve estar no intervalo (0, 100]. Valor: "
                  << motion_threshold_percent << "\n";
        return 7;
    }

    double decoded_frame_index_value = -1.0;
    if (!video_reader->get(
            cv::cudacodec::VideoReaderProps::PROP_DECODED_FRAME_IDX,
            decoded_frame_index_value)) {
        std::cerr << "[fatal] Nao foi possivel obter PROP_DECODED_FRAME_IDX.\n";
        return 8;
    }

    double raw_package_base_index_value = -1.0;
    if (!video_reader->get(
            cv::cudacodec::VideoReaderProps::PROP_RAW_PACKAGES_BASE_INDEX,
            raw_package_base_index_value)) {
        std::cerr << "[fatal] Nao foi possivel obter PROP_RAW_PACKAGES_BASE_INDEX.\n";
        return 9;
    }

    const size_t decoded_frame_index =
        static_cast<size_t>(decoded_frame_index_value);
    const size_t raw_package_base_index =
        static_cast<size_t>(raw_package_base_index_value);

    const int64_t frame_pixel_count =
        static_cast<int64_t>(frame_width) * static_cast<int64_t>(frame_height);
    const double motion_threshold_ratio = motion_threshold_percent / 100.0;

    const double warmup_seconds = 1.0;
    const uint64_t warmup_frame_count =
        static_cast<uint64_t>(stream_fps * warmup_seconds + 0.5);

    std::cout << "STREAM_FPS fps=" << stream_fps << "\n";
    std::cout << "STREAM_RESOLUTION width=" << frame_width
              << " height=" << frame_height << "\n";
    std::cout << "MOTION_THRESHOLD percent=" << motion_threshold_percent << "\n";
    std::cout << "MOG2_WARMUP frames=" << warmup_frame_count << "\n";
    std::cout << "PRE_EVENT_BUFFER target_seconds=" << pre_event_seconds << "\n";
    std::cout << "POST_EVENT target_seconds=" << post_event_seconds << "\n";
    std::cout.flush();

    const int mog2_history = 500;
    const double mog2_variance_threshold = 16.0;
    auto mog2_detector = cv::cuda::createBackgroundSubtractorMOG2(
        mog2_history,
        mog2_variance_threshold,
        false
    );
    mog2_detector->setDetectShadows(false);

    cv::Mat morphology_kernel =
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    auto morphology_filter = cv::cuda::createMorphologyFilter(
        cv::MORPH_OPEN,
        CV_8UC1,
        morphology_kernel
    );

    cv::cuda::GpuMat decoded_frame_gpu;
    cv::cuda::GpuMat motion_mask_gpu;
    cv::cuda::Stream cuda_stream;

    // Buffer circular que mantém apenas os segundos anteriores ao evento.
    std::deque<EncodedPacket> pre_event_buffer;
    size_t pre_event_buffer_bytes = 0;

    // Buffer temporário do evento completo: pré-evento + movimento + pós-evento.
    std::deque<EncodedPacket> event_packet_buffer;
    size_t event_buffer_bytes = 0;
    bool event_collecting = false;
    bool waiting_for_post_event = false;
    std::chrono::steady_clock::time_point post_event_started_at;

    bool motion_active = false;
    int motion_frame_count = 0;
    int idle_frame_count = 0;
    uint64_t frame_index = 0;

    while (keep_running) {
        bool frame_grabbed = false;
        try {
            frame_grabbed = video_reader->grab(cuda_stream);
        } catch (const cv::Exception& error) {
            std::cerr << "[fatal] NVDEC grab exception: " << error.what() << "\n";
            break;
        }

        if (!frame_grabbed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        bool frame_retrieved = false;
        try {
            frame_retrieved = video_reader->retrieve(
                decoded_frame_gpu,
                decoded_frame_index
            );
        } catch (const cv::Exception& error) {
            std::cerr << "[fatal] NVDEC retrieve exception: " << error.what() << "\n";
            break;
        }

        if (!frame_retrieved || decoded_frame_gpu.empty()) {
            std::cerr << "[fatal] Frame decodificado nao foi recuperado.\n";
            break;
        }

        double raw_package_count_value = 0.0;
        if (!video_reader->get(
                cv::cudacodec::VideoReaderProps::PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB,
                raw_package_count_value)) {
            std::cerr << "[fatal] Nao foi possivel obter a quantidade de pacotes codificados.\n";
            break;
        }

        const int raw_package_count =
            static_cast<int>(raw_package_count_value);
        const auto packet_time = std::chrono::steady_clock::now();

        for (int package_offset = 0; package_offset < raw_package_count; ++package_offset) {
            const size_t package_index =
                raw_package_base_index + static_cast<size_t>(package_offset);

            cv::Mat raw_packet;
            if (!video_reader->retrieve(raw_packet, package_index) || raw_packet.empty()) {
                continue;
            }

            double key_frame_value = static_cast<double>(package_index);
            bool has_key_frame = false;
            if (video_reader->get(
                    cv::cudacodec::VideoReaderProps::PROP_LRF_HAS_KEY_FRAME,
                    key_frame_value)) {
                has_key_frame = key_frame_value != 0.0;
            }

            const size_t packet_size_bytes = raw_packet.total() * raw_packet.elemSize();

            EncodedPacket buffered_packet;
            buffered_packet.data.assign(
                raw_packet.data,
                raw_packet.data + packet_size_bytes
            );
            buffered_packet.received_at = packet_time;
            buffered_packet.has_key_frame = has_key_frame;

            // Depois que um evento começou, cada novo pacote também pertence ao evento.
            if (event_collecting) {
                event_buffer_bytes += buffered_packet.data.size();
                event_packet_buffer.push_back(buffered_packet);
            }

            pre_event_buffer_bytes += buffered_packet.data.size();
            pre_event_buffer.push_back(std::move(buffered_packet));
        }

        const auto current_time = std::chrono::steady_clock::now();

        // Mantém no buffer circular somente a janela de pré-evento configurada.
        while (!pre_event_buffer.empty()) {
            const double packet_age_seconds =
                std::chrono::duration<double>(
                    current_time - pre_event_buffer.front().received_at
                ).count();

            if (packet_age_seconds <= pre_event_seconds) {
                break;
            }

            pre_event_buffer_bytes -= pre_event_buffer.front().data.size();
            pre_event_buffer.pop_front();
        }

        mog2_detector->apply(
            decoded_frame_gpu,
            motion_mask_gpu,
            mog2_learning_rate,
            cuda_stream
        );
        cv::cuda::threshold(
            motion_mask_gpu,
            motion_mask_gpu,
            200,
            255,
            cv::THRESH_BINARY,
            cuda_stream
        );
        morphology_filter->apply(motion_mask_gpu, motion_mask_gpu, cuda_stream);
        cuda_stream.waitForCompletion();

        const int64_t foreground_pixel_count = cv::cuda::countNonZero(motion_mask_gpu);

        if (frame_index < warmup_frame_count) {
            motion_frame_count = 0;
            idle_frame_count = 0;
            ++frame_index;
            continue;
        }

        const double foreground_ratio =
            static_cast<double>(foreground_pixel_count) /
            static_cast<double>(frame_pixel_count);
        const bool motion_detected = foreground_ratio >= motion_threshold_ratio;

        if (!motion_active) {
            if (motion_detected) {
                if (++motion_frame_count >= motion_start_frames) {
                    motion_active = true;
                    motion_frame_count = 0;
                    std::cout << "MOTION_ON frame=" << frame_index << "\n";

                    if (!event_collecting) {
                        event_packet_buffer.clear();
                        event_buffer_bytes = 0;

                        // Começa o evento no primeiro key frame disponível dentro
                        // da janela de pré-evento, para manter um início decodificável.
                        auto event_start = std::find_if(
                            pre_event_buffer.begin(),
                            pre_event_buffer.end(),
                            [](const EncodedPacket& packet) {
                                return packet.has_key_frame;
                            }
                        );

                        const bool starts_with_key_frame =
                            event_start != pre_event_buffer.end();

                        if (!starts_with_key_frame) {
                            event_start = pre_event_buffer.begin();
                            std::cout << "EVENT_BUFFER_WARNING no_key_frame_in_pre_event\n";
                        }

                        for (auto packet_it = event_start;
                             packet_it != pre_event_buffer.end();
                             ++packet_it) {
                            event_buffer_bytes += packet_it->data.size();
                            event_packet_buffer.push_back(*packet_it);
                        }

                        event_collecting = true;
                        waiting_for_post_event = false;

                        double pre_event_duration = 0.0;
                        if (!event_packet_buffer.empty()) {
                            pre_event_duration =
                                std::chrono::duration<double>(
                                    current_time - event_packet_buffer.front().received_at
                                ).count();
                        }

                        std::cout << "EVENT_START pre_event_packets="
                                  << event_packet_buffer.size()
                                  << " pre_event_bytes=" << event_buffer_bytes
                                  << " pre_event_duration_seconds=" << pre_event_duration
                                  << " starts_with_key_frame="
                                  << (starts_with_key_frame ? 1 : 0) << "\n";
                    } else if (waiting_for_post_event) {
                        waiting_for_post_event = false;
                        std::cout << "EVENT_CONTINUE reason=motion_resumed_before_post_event_end\n";
                    }

                    std::cout.flush();
                }
            } else {
                motion_frame_count = 0;
            }
        } else {
            if (!motion_detected) {
                if (++idle_frame_count >= motion_end_frames) {
                    motion_active = false;
                    idle_frame_count = 0;
                    waiting_for_post_event = true;
                    post_event_started_at = current_time;

                    std::cout << "MOTION_OFF frame=" << frame_index << "\n";
                    std::cout << "POST_EVENT_WAIT seconds=" << post_event_seconds << "\n";
                    std::cout.flush();
                }
            } else {
                idle_frame_count = 0;
            }
        }

        if (event_collecting && waiting_for_post_event && !motion_active) {
            const double post_event_elapsed =
                std::chrono::duration<double>(
                    current_time - post_event_started_at
                ).count();

            if (post_event_elapsed >= post_event_seconds) {
                double event_duration_seconds = 0.0;
                if (event_packet_buffer.size() >= 2) {
                    event_duration_seconds =
                        std::chrono::duration<double>(
                            event_packet_buffer.back().received_at -
                            event_packet_buffer.front().received_at
                        ).count();
                }

                size_t event_key_frame_count = 0;
                for (const EncodedPacket& packet : event_packet_buffer) {
                    if (packet.has_key_frame) {
                        ++event_key_frame_count;
                    }
                }

                std::cout << "EVENT_COMPLETE packets=" << event_packet_buffer.size()
                          << " bytes=" << event_buffer_bytes
                          << " duration_seconds=" << event_duration_seconds
                          << " key_frames=" << event_key_frame_count << "\n";
                std::cout.flush();

                event_packet_buffer.clear();
                event_buffer_bytes = 0;
                event_collecting = false;
                waiting_for_post_event = false;
            }
        }

        ++frame_index;
    }

    if (motion_active) {
        std::cout << "MOTION_OFF frame=" << frame_index << "\n";
    }

    if (event_collecting) {
        std::cout << "EVENT_BUFFER_ON_SHUTDOWN packets=" << event_packet_buffer.size()
                  << " bytes=" << event_buffer_bytes << "\n";
    }

    std::cout.flush();
    return 0;
}
