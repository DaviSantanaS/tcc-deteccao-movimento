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
#include <iostream>

std::atomic<bool> keep_running{true};
void handle_signal(int){ keep_running = false; }

int main(int argc, char** argv) {
    // args: <rtsp_url> [motion_threshold_percent] [motion_start_frames] [motion_end_frames]
    std::string rtsp_url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double motion_threshold_percent = (argc > 2) ? std::stod(argv[2]) : 1.0;
    int motion_start_frames = (argc > 3) ? std::stoi(argv[3]) : 2;
    int motion_end_frames   = (argc > 4) ? std::stoi(argv[4]) : 3;
    const double mog2_learning_rate = 0.01;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
        return 1;
    }
    cv::cuda::setDevice(0);

    cv::cudacodec::VideoReaderInitParams reader_params;
    reader_params.allowFrameDrop = true;
    reader_params.rawMode = false;
    reader_params.udpSource = true;

    std::vector<int> source_params;
    cv::Ptr<cv::cudacodec::VideoReader> video_reader;
    try {
        video_reader = cv::cudacodec::createVideoReader(rtsp_url, source_params, reader_params);
    } catch (const cv::Exception& error) {
        std::cerr << "[fatal] Falha ao abrir NVDEC: " << error.what() << "\n";
        return 2;
    }

    const cv::cudacodec::FormatInfo stream_format = video_reader->format();
    const double stream_fps = stream_format.fps;
    const int frame_width = stream_format.width;
    const int frame_height = stream_format.height;

    if (stream_fps <= 0.0) {
        std::cerr << "[fatal] FPS invalido informado pelo stream: " << stream_fps << "\n";
        return 3;
    }

    if (frame_width <= 0 || frame_height <= 0) {
        std::cerr << "[fatal] Resolucao invalida informada pelo stream: "
                  << frame_width << "x" << frame_height << "\n";
        return 4;
    }

    if (motion_threshold_percent <= 0.0 || motion_threshold_percent > 100.0) {
        std::cerr << "[fatal] motion_threshold_percent deve estar no intervalo (0, 100]. Valor: "
                  << motion_threshold_percent << "\n";
        return 5;
    }

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

    bool motion_active = false;
    int motion_frame_count = 0;
    int idle_frame_count = 0;
    uint64_t frame_index = 0;

    while (keep_running) {
        bool frame_received = false;
        try {
            frame_received = video_reader->nextFrame(decoded_frame_gpu, cuda_stream);
        } catch (const cv::Exception& error) {
            std::cerr << "[fatal] NVDEC nextFrame exception: " << error.what() << "\n";
            break;
        }

        if (!frame_received) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
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
                    std::cout << "MOTION_OFF frame=" << frame_index << "\n";
                    std::cout.flush();
                }
            } else {
                idle_frame_count = 0;
            }
        }

        ++frame_index;
    }

    if (motion_active) {
        std::cout << "MOTION_OFF frame=" << frame_index << "\n";
        std::cout.flush();
    }

    return 0;
}
