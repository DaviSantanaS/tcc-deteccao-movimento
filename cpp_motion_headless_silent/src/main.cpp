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
#include <iostream> // apenas para erros e transicoes de movimento

std::atomic<bool> running{true};
void handle_sig(int){ running = false; }

int main(int argc, char** argv) {
    // args: <url> [threshold] [start_frames] [end_frames]
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    int threshold = (argc > 2) ? std::stoi(argv[2]) : 5000; // pixels
    int start_frames = (argc > 3) ? std::stoi(argv[3]) : 2;
    int end_frames   = (argc > 4) ? std::stoi(argv[4]) : 3;
    const double mog_lr = 0.01; // learning rate for MOG2

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

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

    // O FPS passa a vir do proprio stream aberto pelo VideoReader.
    const cv::cudacodec::FormatInfo format = reader->format();
    const double fps = format.fps;

    if (fps <= 0.0) {
        std::cerr << "[fatal] FPS invalido informado pelo stream: " << fps << "\n";
        return 3;
    }

    // Um segundo equivalente de frames e reservado para o MOG2 aprender o fundo
    // antes de permitir transicoes MOTION_ON/OFF.
    const double warmup_seconds = 1.0;
    const uint64_t warmup_frames = static_cast<uint64_t>(fps * warmup_seconds + 0.5);

    std::cout << "STREAM_FPS fps=" << fps << "\n";
    std::cout << "MOG2_WARMUP frames=" << warmup_frames << "\n";
    std::cout.flush();

    // Subtrator de fundo (GPU)
    auto mog2 = cv::cuda::createBackgroundSubtractorMOG2(500, 16.0, false);
    mog2->setDetectShadows(false);

    // Morfologia (abertura) na GPU - kernel 3x3
    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3,3));
    auto morphOpen = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, k3);

    cv::cuda::GpuMat d_bgr, d_mask;
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

        // Todo o processamento na GPU:
        mog2->apply(d_bgr, d_mask, mog_lr, stream);                    // MOG2
        cv::cuda::threshold(d_mask, d_mask, 200, 255, cv::THRESH_BINARY, stream); // binariza
        morphOpen->apply(d_mask, d_mask, stream);                      // abertura
        stream.waitForCompletion();

        // conta non-zero (executa na GPU, retorna um inteiro no host)
        // OBS: isso NÃO baixa o frame; só retorna um contador. Mantido para manter a lógica.
        int64_t nz = cv::cuda::countNonZero(d_mask);

        // Durante o warm-up o modelo de fundo continua aprendendo normalmente,
        // mas nenhuma transicao de movimento e emitida.
        if (frame_idx < warmup_frames) {
            consec_motion = 0;
            consec_idle = 0;
            ++frame_idx;
            continue;
        }

        // lógica de debounce: apenas informa a mudança de estado no próprio processo
        if (!in_motion) {
            if (nz > threshold) {
                if (++consec_motion >= start_frames) {
                    in_motion = true;
                    consec_motion = 0;
                    std::cout << "MOTION_ON frame=" << frame_idx << "\n";
                    std::cout.flush();
                }
            } else {
                consec_motion = 0;
            }
        } else { // em movimento
            if (nz <= threshold) {
                if (++consec_idle >= end_frames) {
                    in_motion = false;
                    consec_idle = 0;
                    std::cout << "MOTION_OFF frame=" << frame_idx << "\n";
                    std::cout.flush();
                }
            } else {
                consec_idle = 0;
            }
        }

        ++frame_idx;
    }

    if (in_motion) {
        std::cout << "MOTION_OFF frame=" << frame_idx << "\n";
        std::cout.flush();
    }

    return 0;
}
