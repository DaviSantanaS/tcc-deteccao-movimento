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

std::atomic<bool> running{true};
void handle_sig(int){ running = false; }

int main(int argc, char** argv) {
    // args: <url> [motion_percent] [start_frames] [end_frames] [diff_threshold] [blur_ksize] [blur_sigma]
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double motion_percent = (argc > 2) ? std::stod(argv[2]) : 1.0;
    int start_frames = (argc > 3) ? std::stoi(argv[3]) : 2;
    int end_frames = (argc > 4) ? std::stoi(argv[4]) : 3;
    int diff_threshold = (argc > 5) ? std::stoi(argv[5]) : 30; // diferenca de intensidade: 0..255
    int blur_ksize = (argc > 6) ? std::stoi(argv[6]) : 5;
    double blur_sigma = (argc > 7) ? std::stod(argv[7]) : 0.0;

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visivel.\n";
        return 1;
    }
    cv::cuda::setDevice(0);

    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode = false;
    p.udpSource = true;

    std::vector<int> sourceParams;
    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, sourceParams, p);
    } catch (const cv::Exception& e) {
        std::cerr << "[fatal] Falha ao abrir NVDEC: " << e.what() << "\n";
        return 2;
    }

    const cv::cudacodec::FormatInfo format = reader->format();
    const double fps = format.fps;
    const int width = format.width;
    const int height = format.height;

    if (fps <= 0.0) {
        std::cerr << "[fatal] FPS invalido informado pelo stream: " << fps << "\n";
        return 3;
    }

    if (width <= 0 || height <= 0) {
        std::cerr << "[fatal] Resolucao invalida informada pelo stream: "
                  << width << "x" << height << "\n";
        return 4;
    }

    if (motion_percent <= 0.0 || motion_percent > 100.0) {
        std::cerr << "[fatal] motion_percent deve estar no intervalo (0, 100]. Valor: "
                  << motion_percent << "\n";
        return 5;
    }

    if (diff_threshold < 0 || diff_threshold > 255) {
        std::cerr << "[fatal] diff_threshold deve estar no intervalo [0, 255]. Valor: "
                  << diff_threshold << "\n";
        return 6;
    }

    const int64_t total_pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);

    const int ksize = std::max(3, blur_ksize | 1);
    auto gauss = cv::cuda::createGaussianFilter(
        CV_8UC1,
        CV_8UC1,
        cv::Size(ksize, ksize),
        blur_sigma
    );

    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    auto morphOpen = cv::cuda::createMorphologyFilter(
        cv::MORPH_OPEN,
        CV_8UC1,
        k3
    );

    std::cout << "STREAM_FPS fps=" << fps << "\n";
    std::cout << "STREAM_RESOLUTION width=" << width << " height=" << height << "\n";
    std::cout << "MOTION_THRESHOLD percent=" << motion_percent << "\n";
    std::cout << "PIXEL_DIFF_THRESHOLD value=" << diff_threshold << "\n";
    std::cout.flush();

    cv::cuda::GpuMat d_src;
    cv::cuda::GpuMat d_gray;
    cv::cuda::GpuMat d_prev_gray;
    cv::cuda::GpuMat d_diff;
    cv::cuda::GpuMat d_blur;
    cv::cuda::GpuMat d_mask;
    cv::cuda::Stream stream;

    bool in_motion = false;
    int consec_motion = 0;
    int consec_idle = 0;
    uint64_t frame_idx = 0;

    while (running) {
        bool ok = false;
        try {
            ok = reader->nextFrame(d_src, stream); // NVDEC -> GpuMat
        } catch (const cv::Exception& e) {
            std::cerr << "[fatal] NVDEC nextFrame exception: " << e.what() << "\n";
            break;
        }

        if (!ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const int type = d_src.type();
        const int depth = CV_MAT_DEPTH(type);
        const int channels = CV_MAT_CN(type);

        if (depth != CV_8U) {
            std::cerr << "[fatal] Formato inesperado: frame nao e 8-bit. type="
                      << type << " channels=" << channels << "\n";
            break;
        }

        // A diferenca e calculada em escala de cinza, sempre na GPU.
        if (channels == 4) {
            cv::cuda::cvtColor(d_src, d_gray, cv::COLOR_BGRA2GRAY, 0, stream);
        } else if (channels == 3) {
            cv::cuda::cvtColor(d_src, d_gray, cv::COLOR_BGR2GRAY, 0, stream);
        } else if (channels == 1) {
            // Se o decoder entregar NV12, usamos apenas o plano Y (luminancia) como cinza.
            const int expected_nv12_rows = height + (height / 2);
            if (d_src.cols == width && d_src.rows == expected_nv12_rows) {
                cv::cuda::GpuMat y_plane(d_src, cv::Rect(0, 0, width, height));
                y_plane.copyTo(d_gray, stream);
            } else if (d_src.cols == width && d_src.rows == height) {
                d_src.copyTo(d_gray, stream);
            } else {
                std::cerr << "[fatal] Frame de 1 canal com dimensoes inesperadas: "
                          << d_src.cols << "x" << d_src.rows << "\n";
                break;
            }
        } else {
            std::cerr << "[fatal] Quantidade inesperada de canais: " << channels << "\n";
            break;
        }

        // A primeira imagem apenas inicializa o frame anterior.
        if (d_prev_gray.empty()) {
            d_gray.copyTo(d_prev_gray, stream);
            stream.waitForCompletion();
            ++frame_idx;
            continue;
        }

        // Diferenca absoluta entre frame atual e frame anterior.
        cv::cuda::absdiff(d_gray, d_prev_gray, d_diff, stream);

        // Suaviza pequenas variacoes causadas por ruido/compressao.
        gauss->apply(d_diff, d_blur, stream);

        // Um pixel so entra na mascara se sua intensidade mudou mais que diff_threshold.
        cv::cuda::threshold(
            d_blur,
            d_mask,
            diff_threshold,
            255,
            cv::THRESH_BINARY,
            stream
        );

        // Remove pequenos pontos isolados da mascara.
        morphOpen->apply(d_mask, d_mask, stream);
        stream.waitForCompletion();

        const int64_t changed_pixels = cv::cuda::countNonZero(d_mask);
        const double changed_percent =
            (static_cast<double>(changed_pixels) / static_cast<double>(total_pixels)) * 100.0;

        const bool has_motion = changed_percent >= motion_percent;

        if (!in_motion) {
            if (has_motion) {
                if (++consec_motion >= start_frames) {
                    in_motion = true;
                    consec_motion = 0;
                    std::cout << "MOTION_ON frame=" << frame_idx
                              << " changed_percent=" << changed_percent << "\n";
                    std::cout.flush();
                }
            } else {
                consec_motion = 0;
            }
        } else {
            if (!has_motion) {
                if (++consec_idle >= end_frames) {
                    in_motion = false;
                    consec_idle = 0;
                    std::cout << "MOTION_OFF frame=" << frame_idx
                              << " changed_percent=" << changed_percent << "\n";
                    std::cout.flush();
                }
            } else {
                consec_idle = 0;
            }
        }

        // O frame atual vira a referencia para a proxima iteracao.
        d_gray.copyTo(d_prev_gray, stream);
        ++frame_idx;
    }

    if (in_motion) {
        std::cout << "MOTION_OFF frame=" << frame_idx << "\n";
        std::cout.flush();
    }

    return 0;
}
