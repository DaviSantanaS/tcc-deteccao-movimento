#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>

#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <csignal>
#include <atomic>
#include <thread>
#include <string>

std::atomic<bool> running{true};
void handle_sig(int){ running = false; }

std::string now_iso_seconds() {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

int main(int argc, char** argv) {
    // args: <url> [fps] [threshold] [start_frames] [end_frames]
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double fps = (argc > 2) ? std::stod(argv[2]) : 30.0;
    int threshold = (argc > 3) ? std::stoi(argv[3]) : 5000; // pixels
    int start_frames = (argc > 4) ? std::stoi(argv[4]) : 2;
    int end_frames   = (argc > 5) ? std::stoi(argv[5]) : 3;
    double mog_lr = 0.01; // learning rate for MOG2

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    // Verifica GPU CUDA
    int devs = cv::cuda::getCudaEnabledDeviceCount();
    if (devs <= 0) {
        std::cerr << "Nenhuma GPU CUDA visível.\n";
        return 1;
    }
    cv::cuda::setDevice(0);
    cv::cuda::printShortCudaDeviceInfo(0);

    // NVDEC via cudacodec — flags para stream ao vivo
    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode = false;
    p.udpSource = true;

    std::vector<int> sourceParams; // vazio
    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, sourceParams, p);
        std::cerr << "[reader] cudacodec OK\n";
    } catch (const cv::Exception& e) {
        std::cerr << "[reader] falha cudacodec: " << e.what() << "\n";
        return 2;
    }

    // Subtrator de fundo (GPU)
    auto mog2 = cv::cuda::createBackgroundSubtractorMOG2(500, 16.0, false);
    mog2->setDetectShadows(false);

    // Morfologia (abertura) na GPU - kernel 3x3
    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3,3));
    auto morphOpen = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, k3);

    cv::cuda::GpuMat d_bgr, d_mask;
    cv::cuda::Stream stream;

    std::cout << "Headless detector iniciado. URL=" << url
              << " fps=" << fps << " threshold=" << threshold << "\n";

    bool in_motion = false;
    int consec_motion = 0, consec_idle = 0;
    uint64_t frame_idx = 0;

    while (running) {
        bool ok = false;
        try {
            ok = reader->nextFrame(d_bgr, stream);
        } catch (const cv::Exception& e) {
            std::cerr << "NVDEC nextFrame exception: " << e.what() << "\n";
            break;
        }
        if (!ok) {
            // sem frame no momento; evita busy-loop
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Processo todo na GPU:
        // 1) MOG2 -> d_mask (GPU)
        mog2->apply(d_bgr, d_mask, mog_lr, stream);

        // 2) threshold binário -> d_mask (GPU)
        cv::cuda::threshold(d_mask, d_mask, 200, 255, cv::THRESH_BINARY, stream);

        // 3) morph open (GPU)
        morphOpen->apply(d_mask, d_mask, stream);

        // 4) espere completar as operações enfileiradas
        stream.waitForCompletion();

        // 5) conte non-zero (roda na GPU e retorna int para CPU)
        int64_t nz = cv::cuda::countNonZero(d_mask);

        // segundo aproximado do vídeo
        double sec = double(frame_idx) / fps;

        // debounce / lógico de evento (CPU)
        if (!in_motion) {
            if (nz > threshold) {
                consec_motion++;
                consec_idle = 0;
                if (consec_motion >= start_frames) {
                    in_motion = true;
                    consec_motion = 0;
                    std::cout << "[INÍCIO] " << now_iso_seconds()
                              << "  — segundo=" << std::fixed << std::setprecision(2) << sec
                              << "  (nz=" << nz << ")\n";
                }
            } else {
                consec_motion = 0;
            }
        } else {
            if (nz <= threshold) {
                consec_idle++;
                if (consec_idle >= end_frames) {
                    in_motion = false;
                    consec_idle = 0;
                    std::cout << "[FIM]   " << now_iso_seconds()
                              << "  — segundo=" << std::fixed << std::setprecision(2) << sec
                              << "  (nz=" << nz << ")\n";
                }
            } else {
                consec_idle = 0;
            }
        }

        frame_idx++;
    }

    // Se terminar com evento aberto, marca fim com tempo atual
    if (in_motion) {
        double sec = double(frame_idx) / fps;
        std::cout << "[FIM]   " << now_iso_seconds()
                  << "  — segundo=" << std::fixed << std::setprecision(2) << sec
                  << "  (forced end on shutdown)\n";
    }

    std::cout << "Encerrando detector.\n";
    return 0;
}
