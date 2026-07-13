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
#include <iostream> // apenas para erros
#include "MotionBus.hpp"
#include "motion/Mog2Detector.hpp"

std::atomic<bool> running{true};
void handle_sig(int){ running = false; }

int main(int argc, char** argv) {
    // args: <url> [fps] [threshold] [start_frames] [end_frames]
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    double fps = (argc > 2) ? std::stod(argv[2]) : 30.0;
    int threshold = (argc > 3) ? std::stoi(argv[3]) : 5000; // pixels
    int start_frames = (argc > 4) ? std::stoi(argv[4]) : 2;
    int end_frames   = (argc > 5) ? std::stoi(argv[5]) : 3;
    const double mog_lr = 0.01; // learning rate for MOG2

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    MotionBus bus("/tmp/motion_bus");


    // GPU disponível?
    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "[fatal] Nenhuma GPU CUDA visível.\n";
        return 1;
    }
    cv::cuda::setDevice(0);

    // NVDEC via cudacodec — flags para stream ao vivo
    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode = false;
    p.udpSource = true;

    std::vector<int> sourceParams; // vazio
    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, sourceParams, p);
    } catch (const cv::Exception& e) {
        std::cerr << "[fatal] Falha ao abrir NVDEC: " << e.what() << "\n";
        return 2;
    }

    if (!reader->set(cv::cudacodec::ColorFormat::BGRA)) {
        std::cerr << "[fatal] NVDEC nao suporta saida BGRA.\n";
        return 2;
    }

    motion::Mog2Config detectorConfig;
    detectorConfig.history = 500;
    detectorConfig.varianceThreshold = 16.0;
    detectorConfig.detectShadows = false;
    detectorConfig.learningRate = mog_lr;
    detectorConfig.maskThreshold = 200.0;
    detectorConfig.morphologyKernelSize = 3;

    motion::Mog2Detector detector(detectorConfig);

    cv::cuda::GpuMat d_bgr;
    cv::cuda::Stream stream;

    bool in_motion = false;
    int consec_motion = 0, consec_idle = 0;
    uint64_t frame_idx = 0;

    while (running) {
        bool ok = false;
        try {
            ok = reader->nextFrame(d_bgr, stream); // NVDEC -> GpuMat (sem CPU)
        } catch (const cv::Exception& e) {
            std::cerr << "[fatal] NVDEC nextFrame exception: " << e.what() << "\n";
            break;
        }
        if (!ok) { // sem frame no momento; evita busy-loop
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const motion::DetectionResult result = detector.process(d_bgr, stream);

        if (!result.valid) {
            continue;
        }

        const int64_t nz = result.activePixels;
        if (!in_motion) {
            if (nz > threshold) {
                if (++consec_motion >= start_frames) {
                    in_motion = true;
                    consec_motion = 0;
                                    bus.on();
}
            } else {
                consec_motion = 0;
            }
        } else { // em movimento
            if (nz <= threshold) {
                if (++consec_idle >= end_frames) {
                    in_motion = false;
                    consec_idle = 0;
                                    bus.off();
}
            } else {
                consec_idle = 0;
            }
        }

        ++frame_idx;
    }

    if (in_motion) bus.off();

    return 0;
}
