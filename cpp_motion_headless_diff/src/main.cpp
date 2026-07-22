#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include "MotionBus.hpp"
#include "motion/FrameDifferenceDetector.hpp"

#include <csignal>
#include <atomic>
#include <thread>
#include <string>
#include <iostream>

static std::atomic<bool> running{true};
static void handle_sig(int){ running = false; }

int main(int argc, char** argv) {
    // args: <url> [fps] [nz_thr] [startN] [endN] [diff_thr] [blur_ksize] [blur_sigma]
    std::string url      = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double      fps      = (argc > 2) ? std::stod(argv[2]) : 30.0;
    int         nz_thr   = (argc > 3) ? std::stoi(argv[3]) : 5000;
    int         startN   = (argc > 4) ? std::stoi(argv[4]) : 2;
    int         endN     = (argc > 5) ? std::stoi(argv[5]) : 3;
    int         diff_thr = (argc > 6) ? std::stoi(argv[6]) : 30;      // 0..255
    int         ksize    = (argc > 7) ? std::stoi(argv[7]) : 5;       // 3/5/7 (ímpar)
    double      sigma    = (argc > 8) ? std::stod(argv[8]) : 0.0;

    std::signal(SIGINT,  handle_sig);
    std::signal(SIGTERM, handle_sig);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n"; return 1;
    }
    cv::cuda::setDevice(0);
    cv::setNumThreads(1); // reduz overhead no host

    // NVDEC (cudacodec)
    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode        = false;
    p.udpSource      = true;

    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, {}, p);
    } catch (const cv::Exception& e) {
        std::cerr << "[fatal] Falha ao abrir NVDEC: " << e.what() << "\n"; return 2;
    }

    if (!reader->set(cv::cudacodec::ColorFormat::BGRA)) {
        std::cerr << "[fatal] NVDEC nao suporta saida BGRA.\n";
        return 2;
    }

    motion::FrameDifferenceConfig detectorConfig;
    detectorConfig.differenceThreshold = diff_thr;
    detectorConfig.gaussianKernelSize = ksize;
    detectorConfig.gaussianSigma = sigma;
    detectorConfig.morphologyKernelSize = 3;

    motion::FrameDifferenceDetector detector(detectorConfig);

    cv::cuda::GpuMat d_src;
    cv::cuda::Stream stream;

    bool in_motion=false; int consecOn=0, consecOff=0;
    uint64_t frame_idx=0;
    MotionBus bus;

    while (running) {
        bool ok=false;
        try {
            ok = reader->nextFrame(d_src, stream);
        } catch (const cv::Exception& e) {
            std::cerr << "[fatal] NVDEC nextFrame exception: " << e.what() << "\n"; break;
        }
        if (!ok) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

        const motion::DetectionResult result = detector.process(d_src, stream);

        if (!result.valid) {
            continue;
        }

        const int64_t nz = result.activePixels;

        // Debounce com emissão somente nas mudanças de estado
        if (!in_motion) {
            if (nz > nz_thr) {
                if (++consecOn >= startN) {
                    in_motion = true;
                    consecOn = 0;
                    bus.on();
                }
            } else {
                consecOn = 0;
            }
        } else {
            if (nz <= nz_thr) {
                if (++consecOff >= endN) {
                    in_motion = false;
                    consecOff = 0;
                    bus.off();
                }
            } else {
                consecOff = 0;
            }
        }

        ++frame_idx;
        // opcional: aliviar CPU
        // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
