#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>

std::atomic<bool> running{true};
void sigint_handler(int){ running = false; }

int main(int argc, char** argv) {
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";
    // parâmetros opcionais:
    int motion_threshold = (argc > 2) ? std::stoi(argv[2]) : 5000; // pixels
    double mog_lr = 0.01; // learning rate passado para apply
    int wait_ms = 1; // waitKey

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // Verifica GPU CUDA
    int devs = cv::cuda::getCudaEnabledDeviceCount();
    if (devs <= 0) { std::cerr << "Nenhuma GPU CUDA visível.\n"; return 1; }
    cv::cuda::setDevice(0);
    cv::cuda::printShortCudaDeviceInfo(0);

    // NVDEC via cudacodec — flags para stream ao vivo
    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode        = false;
    p.udpSource      = true;   // evita alguns erros de parser em RTSP live

    std::vector<int> sourceParams; // vazio
    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, sourceParams, p);
        std::cerr << "[reader] cudacodec OK\n";
    } catch (const cv::Exception& e) {
        std::cerr << "Falha ao abrir NVDEC: " << e.what() << "\n";
        return 2;
    }

    // Subtrator de fundo (GPU)
    auto mog2 = cv::cuda::createBackgroundSubtractorMOG2(500, 16.0, false);
    mog2->setDetectShadows(false);

    // Morfologia (abertura) na GPU
    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3,3));
    auto morphOpen = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, k3);

    cv::cuda::GpuMat d_bgr, d_mask;
    cv::cuda::Stream stream;
    cv::Mat frameCPU;

    std::cout << "Iniciando detecção (ESC/q para sair)... URL=" << url << "\n";
    uint64_t frame_idx = 0;
    bool in_motion = false;
    int consec_start = 0, consec_end = 0;
    const int start_frames = 2; // debounce p/ início
    const int end_frames   = 3; // debounce p/ fim

    for (;;) {
        if (!running.load()) break;

        bool ok = false;
        try {
            ok = reader->nextFrame(d_bgr, stream);
        } catch (const cv::Exception& e) {
            std::cerr << "NVDEC nextFrame exception: " << e.what() << "\n";
            break;
        }
        if (!ok) break;

        // BG subtract + limpeza (tudo na GPU)
        mog2->apply(d_bgr, d_mask, mog_lr, stream);
        cv::cuda::threshold(d_mask, d_mask, 200, 255, cv::THRESH_BINARY, stream);
        morphOpen->apply(d_mask, d_mask, stream);
        stream.waitForCompletion();

        int nz = cv::cuda::countNonZero(d_mask);
        bool motion = (nz > motion_threshold);  // limiar de pixels "em movimento"

        // Baixa frame para CPU para exibir e desenhar (igual ao seu código original)
        d_bgr.download(frameCPU);
        if (frameCPU.empty()) {
            ++frame_idx;
            continue;
        }

        // lógica de debounce (mesma ideia do seu snippet)
        if (!in_motion) {
            if (motion) {
                ++consec_start;
                consec_end = 0;
                if (consec_start >= start_frames) {
                    in_motion = true;
                    consec_start = 0;
                    std::cout << "[INÍCIO] frame=" << frame_idx << " nz=" << nz << "\n";
                }
            } else {
                consec_start = 0;
            }
        } else {
            if (!motion) {
                ++consec_end;
                if (consec_end >= end_frames) {
                    in_motion = false;
                    consec_end = 0;
                    std::cout << "[FIM]   frame=" << frame_idx << " nz=" << nz << "\n";
                }
            } else {
                consec_end = 0;
            }
        }

        // desenha overlay exatamente como você tinha
        if (in_motion) {
            cv::rectangle(frameCPU, cv::Rect(10,10, frameCPU.cols-20, frameCPU.rows-20), cv::Scalar(0,255,0), 2);
            cv::putText(frameCPU, "MOTION", cv::Point(20,50), cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(0,255,0), 2);
        } else {
            cv::putText(frameCPU, "idle", cv::Point(20,50), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0,200,200), 2);
        }

        // mostra a janela como antes
        cv::imshow("Frame (GPU decoded)", frameCPU);
        int key = cv::waitKey(wait_ms) & 0xFF;
        if (key == 27 || key == 'q') break;

        ++frame_idx;
    }

    std::cout << "Encerrado.\n";
    return 0;
}
