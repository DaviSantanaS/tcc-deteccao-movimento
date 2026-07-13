#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#include <opencv2/cudaimgproc.hpp>
#include "MotionBus.hpp"

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

    // Buffers/filtros na GPU
    cv::cuda::GpuMat d_src, d_gray, d_prevGray, d_diff, d_blur, d_mask;
    cv::cuda::Stream stream;

    int k = std::max(3, ksize | 1);
    cv::Ptr<cv::cuda::Filter> gauss = cv::cuda::createGaussianFilter(CV_8UC1, CV_8UC1, cv::Size(k, k), sigma);
    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3,3));
    cv::Ptr<cv::cuda::Filter> morphOpen = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, k3);

    bool in_motion=false; int consecOn=0, consecOff=0;
    uint64_t frame_idx=0;
    MotionBus bus;

    while (running) {
        bool ok=false;
        try {
            ok = reader->nextFrame(d_src, stream); // pode vir BGR (3ch), BGRA (4ch) ou NV12 (1ch empilhado)
        } catch (const cv::Exception& e) {
            std::cerr << "[fatal] NVDEC nextFrame exception: " << e.what() << "\n"; break;
        }
        if (!ok) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

        const int type  = d_src.type();
        const int depth = CV_MAT_DEPTH(type);
        const int chans = CV_MAT_CN(type);

        if (depth != CV_8U) {
            std::cerr << "[fatal] Formato inesperado (não 8-bit). type="<<type<<" chans="<<chans<<"\n";
            break;
        }

        // ---- Converter para GRAY na GPU conforme o formato recebido ----
        if (chans == 3) {
            // BGR → GRAY
            cv::cuda::cvtColor(d_src, d_gray, cv::COLOR_BGR2GRAY, 0, stream);
        } else if (chans == 4) {
            // BGRA/RGBA → GRAY (BGRA é o comum)
            cv::cuda::cvtColor(d_src, d_gray, cv::COLOR_BGRA2GRAY, 0, stream);
        } else if (chans == 1) {
            // Provável NV12 (rows = H*3/2, cols = W) → usa plano Y como GRAY
            int rows = d_src.rows, cols = d_src.cols;
            if ((rows * 2) % 3 == 0) {
                int H = (rows * 2) / 3;
                if (H > 0 && H <= rows) {
                    cv::cuda::GpuMat y_plane(d_src, cv::Rect(0, 0, cols, H)); // view sem cópia
                    d_gray = y_plane;
                } else {
                    // fallback: tenta conversão NV12->GRAY
                    cv::cuda::cvtColor(d_src, d_gray, cv::COLOR_YUV2GRAY_NV12, 0, stream);
                }
            } else {
                // 1 canal mas não NV12 clássico: assume que já é GRAY
                d_gray = d_src;
            }
        } else {
            std::cerr << "[fatal] Canais inesperados: "<<chans<<"\n";
            break;
        }

        // Primeira iteração: inicializa "prev"
        if (d_prevGray.empty()) {
            d_gray.copyTo(d_prevGray, stream);
            stream.waitForCompletion();
            ++frame_idx;
            continue;
        }

        // |curr - prev| (GPU)
        cv::cuda::absdiff(d_gray, d_prevGray, d_diff, stream);

        // Blur leve (GPU) para ruído/compressão
        gauss->apply(d_diff, d_blur, stream);

        // Threshold (GPU)
        cv::cuda::threshold(d_blur, d_mask, diff_thr, 255, cv::THRESH_BINARY, stream);

        // Morfologia (GPU)
        morphOpen->apply(d_mask, d_mask, stream);

        // Sincroniza antes do contador
        stream.waitForCompletion();

        // Conta pixels ativos (só o escalar vem ao host)
        int64_t nz = cv::cuda::countNonZero(d_mask);

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

        // Atualiza prev
        d_gray.copyTo(d_prevGray, stream);
        ++frame_idx;
        // opcional: aliviar CPU
        // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
