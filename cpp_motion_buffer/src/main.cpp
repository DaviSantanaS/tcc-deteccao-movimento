#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudaarithm.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

static std::atomic<bool> running{true};
static void handle_sig(int){ running = false; }

int main(int argc, char** argv){
    // args: <url> [nz_thr] [startN] [endN] [history] [var_thr] [detect_shadows(0/1)] [learning_rate]
    std::string url     = (argc>1)? argv[1] : "rtsp://127.0.0.1:8554/video";
    int   nz_thr        = (argc>2)? std::stoi(argv[2]) : 5000;
    int   startN        = (argc>3)? std::stoi(argv[3]) : 2;
    int   endN          = (argc>4)? std::stoi(argv[4]) : 3;
    int   history       = (argc>5)? std::stoi(argv[5]) : 200;
    double var_thr      = (argc>6)? std::stod(argv[6]) : 16.0;
    bool  use_shadows   = (argc>7)? (std::stoi(argv[7])!=0) : false; // padrão: sem sombras
    double learningRate = (argc>8)? std::stod(argv[8]) : 0.01;

    std::signal(SIGINT,  handle_sig);
    std::signal(SIGTERM, handle_sig);

    if (cv::cuda::getCudaEnabledDeviceCount() <= 0){
        std::cerr << "[fatal] Nenhuma GPU CUDA.\n";
        return 1;
    }
    cv::cuda::setDevice(0);
    cv::setNumThreads(1);

    // RTSP via NVDEC
    cv::cudacodec::VideoReaderInitParams init;
    init.allowFrameDrop = true;
    init.rawMode = false;
    init.udpSource = true;

    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, {}, init);
        std::cerr << "[reader] NVDEC OK\n";
    } catch(const cv::Exception& e){
        std::cerr << "[fatal] NVDEC open: " << e.what() << "\n";
        return 2;
    }

    // MOG2 (GPU) — sem sombras para máscara 0/255
    auto mog2 = cv::cuda::createBackgroundSubtractorMOG2(history, var_thr, use_shadows);
    mog2->setDetectShadows(use_shadows);

    cv::cuda::GpuMat d_src, d_bgr, d_fg, d_bin;
    cv::cuda::Stream s;

    bool in_motion=false; int on=0, off=0;

    while (running){
        bool ok=false;
        try { ok = reader->nextFrame(d_src, s); }
        catch(const cv::Exception& e){ std::cerr << "[fatal] next: " << e.what() << "\n"; break; }
        if (!ok){ std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

        // Normaliza formato → BGR (NV12/BGRA/BGR)
        int chans = CV_MAT_CN(d_src.type());
        if      (chans==3) d_bgr = d_src;
        else if (chans==4) cv::cuda::cvtColor(d_src, d_bgr, cv::COLOR_BGRA2BGR, 0, s);
        else if (chans==1) cv::cuda::cvtColor(d_src, d_bgr, cv::COLOR_YUV2BGR_NV12, 0, s);
        else { std::cerr << "[fatal] canais="<<chans << "\n"; break; }

        // MOG2 → máscara
        mog2->apply(d_bgr, d_fg, learningRate, s);

        // Binariza mantendo só 255
        cv::cuda::threshold(d_fg, d_bin, 254, 255, cv::THRESH_BINARY, s);
        s.waitForCompletion();

        // Quantidade de pixels "ativos"
        int64_t nz = cv::cuda::countNonZero(d_bin);

        // Debounce simples
        if (!in_motion){
            if (nz > nz_thr){
                if (++on >= startN){ in_motion=true; on=0; std::cout << "MOTION START\n"; std::cout.flush(); }
            } else on=0;
        } else {
            if (nz <= nz_thr){
                if (++off >= endN){ in_motion=false; off=0; std::cout << "MOTION END\n"; std::cout.flush(); }
            } else off=0;
        }
    }

    if (in_motion){ std::cout << "MOTION END\n"; std::cout.flush(); }
    return 0;
}
