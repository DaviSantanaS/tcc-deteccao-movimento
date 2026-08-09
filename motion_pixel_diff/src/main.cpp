#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#include <opencv2/cudaimgproc.hpp>

#include <csignal>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

std::atomic<bool> keep_running{true};
void handle_signal(int){ keep_running = false; }

int main(int argc, char** argv) {
    // args: <rtsp_url> [motion_threshold_percent] [motion_start_frames] [motion_end_frames]
    //       [pixel_difference_threshold] [blur_kernel_size] [blur_sigma]
    std::string rtsp_url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double motion_threshold_percent = (argc > 2) ? std::stod(argv[2]) : 1.0;
    int motion_start_frames = (argc > 3) ? std::stoi(argv[3]) : 2;
    int motion_end_frames = (argc > 4) ? std::stoi(argv[4]) : 3;
    int pixel_difference_threshold = (argc > 5) ? std::stoi(argv[5]) : 30;
    int blur_kernel_size = (argc > 6) ? std::stoi(argv[6]) : 5;
    double blur_sigma = (argc > 7) ? std::stod(argv[7]) : 0.0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visivel.\n";
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

    if (pixel_difference_threshold < 0 || pixel_difference_threshold > 255) {
        std::cerr << "[fatal] pixel_difference_threshold deve estar no intervalo [0, 255]. Valor: "
                  << pixel_difference_threshold << "\n";
        return 6;
    }

    const int64_t frame_pixel_count =
        static_cast<int64_t>(frame_width) * static_cast<int64_t>(frame_height);

    const int normalized_blur_kernel_size = std::max(3, blur_kernel_size | 1);
    auto gaussian_filter = cv::cuda::createGaussianFilter(
        CV_8UC1,
        CV_8UC1,
        cv::Size(normalized_blur_kernel_size, normalized_blur_kernel_size),
        blur_sigma
    );

    cv::Mat morphology_kernel =
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    auto morphology_filter = cv::cuda::createMorphologyFilter(
        cv::MORPH_OPEN,
        CV_8UC1,
        morphology_kernel
    );

    std::cout << "STREAM_FPS fps=" << stream_fps << "\n";
    std::cout << "STREAM_RESOLUTION width=" << frame_width
              << " height=" << frame_height << "\n";
    std::cout << "MOTION_THRESHOLD percent=" << motion_threshold_percent << "\n";
    std::cout << "PIXEL_DIFF_THRESHOLD value=" << pixel_difference_threshold << "\n";
    std::cout.flush();

    cv::cuda::GpuMat decoded_frame_gpu;
    cv::cuda::GpuMat gray_frame_gpu;
    cv::cuda::GpuMat previous_gray_frame_gpu;
    cv::cuda::GpuMat frame_difference_gpu;
    cv::cuda::GpuMat blurred_difference_gpu;
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

        const int frame_type = decoded_frame_gpu.type();
        const int frame_depth = CV_MAT_DEPTH(frame_type);
        const int frame_channels = CV_MAT_CN(frame_type);

        if (frame_depth != CV_8U) {
            std::cerr << "[fatal] Formato inesperado: frame nao e 8-bit. type="
                      << frame_type << " channels=" << frame_channels << "\n";
            break;
        }

        if (frame_channels == 4) {
            cv::cuda::cvtColor(
                decoded_frame_gpu,
                gray_frame_gpu,
                cv::COLOR_BGRA2GRAY,
                0,
                cuda_stream
            );
        } else if (frame_channels == 3) {
            cv::cuda::cvtColor(
                decoded_frame_gpu,
                gray_frame_gpu,
                cv::COLOR_BGR2GRAY,
                0,
                cuda_stream
            );
        } else if (frame_channels == 1) {
            const int expected_nv12_rows = frame_height + (frame_height / 2);
            if (decoded_frame_gpu.cols == frame_width &&
                decoded_frame_gpu.rows == expected_nv12_rows) {
                cv::cuda::GpuMat luminance_plane_gpu(
                    decoded_frame_gpu,
                    cv::Rect(0, 0, frame_width, frame_height)
                );
                luminance_plane_gpu.copyTo(gray_frame_gpu, cuda_stream);
            } else if (decoded_frame_gpu.cols == frame_width &&
                       decoded_frame_gpu.rows == frame_height) {
                decoded_frame_gpu.copyTo(gray_frame_gpu, cuda_stream);
            } else {
                std::cerr << "[fatal] Frame de 1 canal com dimensoes inesperadas: "
                          << decoded_frame_gpu.cols << "x" << decoded_frame_gpu.rows << "\n";
                break;
            }
        } else {
            std::cerr << "[fatal] Quantidade inesperada de canais: "
                      << frame_channels << "\n";
            break;
        }

        if (previous_gray_frame_gpu.empty()) {
            gray_frame_gpu.copyTo(previous_gray_frame_gpu, cuda_stream);
            cuda_stream.waitForCompletion();
            ++frame_index;
            continue;
        }

        cv::cuda::absdiff(
            gray_frame_gpu,
            previous_gray_frame_gpu,
            frame_difference_gpu,
            cuda_stream
        );

        gaussian_filter->apply(
            frame_difference_gpu,
            blurred_difference_gpu,
            cuda_stream
        );

        cv::cuda::threshold(
            blurred_difference_gpu,
            motion_mask_gpu,
            pixel_difference_threshold,
            255,
            cv::THRESH_BINARY,
            cuda_stream
        );

        morphology_filter->apply(motion_mask_gpu, motion_mask_gpu, cuda_stream);
        cuda_stream.waitForCompletion();

        const int64_t changed_pixel_count = cv::cuda::countNonZero(motion_mask_gpu);
        const double changed_pixel_percent =
            (static_cast<double>(changed_pixel_count) /
             static_cast<double>(frame_pixel_count)) * 100.0;

        const bool motion_detected =
            changed_pixel_percent >= motion_threshold_percent;

        if (!motion_active) {
            if (motion_detected) {
                if (++motion_frame_count >= motion_start_frames) {
                    motion_active = true;
                    motion_frame_count = 0;
                    std::cout << "MOTION_ON frame=" << frame_index
                              << " changed_percent=" << changed_pixel_percent << "\n";
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
                    std::cout << "MOTION_OFF frame=" << frame_index
                              << " changed_percent=" << changed_pixel_percent << "\n";
                    std::cout.flush();
                }
            } else {
                idle_frame_count = 0;
            }
        }

        gray_frame_gpu.copyTo(previous_gray_frame_gpu, cuda_stream);
        ++frame_index;
    }

    if (motion_active) {
        std::cout << "MOTION_OFF frame=" << frame_index << "\n";
        std::cout.flush();
    }

    return 0;
}
