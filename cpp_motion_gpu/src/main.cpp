#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#include <iostream>

int main(int argc, char** argv) {
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";

    // GPU disponível?
    int devs = cv::cuda::getCudaEnabledDeviceCount();
    if (devs <= 0) { std::cerr << "Nenhuma GPU CUDA visível.\n"; return 1; }
    cv::cuda::setDevice(0);
    cv::cuda::printShortCudaDeviceInfo(0);

    // NVDEC via cudacodec — flags para stream ao vivo
    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode        = false;
    p.udpSource      = true;   // <<< evita o erro de parser em RTSP live

    std::vector<int> sourceParams; // vazio
    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, sourceParams, p);
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

    std::cout << "Iniciando detecção (ESC para sair)...\n";
    for (;;) {
        bool ok = false;
        try {
            ok = reader->nextFrame(d_bgr, stream);
        } catch (const cv::Exception& e) {
            std::cerr << "NVDEC nextFrame exception: " << e.what() << "\n";
            break;
        }
        if (!ok) break;

        // BG subtract + limpeza (tudo na GPU)
        mog2->apply(d_bgr, d_mask, 0.01, stream);
        cv::cuda::threshold(d_mask, d_mask, 200, 255, cv::THRESH_BINARY, stream);
        morphOpen->apply(d_mask, d_mask, stream);
        stream.waitForCompletion();

        int nz = cv::cuda::countNonZero(d_mask);
        bool motion = (nz > 5000);  // limiar de pixels "em movimento"

        d_bgr.download(frameCPU);
        if (frameCPU.empty()) continue;

        if (motion) {
            cv::rectangle(frameCPU, {10,10, frameCPU.cols-20, frameCPU.rows-20}, {0,255,0}, 2);
            cv::putText(frameCPU, "MOTION", {20,50}, cv::FONT_HERSHEY_SIMPLEX, 1.2, {0,255,0}, 2);
        } else {
            cv::putText(frameCPU, "idle", {20,50}, cv::FONT_HERSHEY_SIMPLEX, 1.0, {0,200,200}, 2);
        }

        cv::imshow("Frame (GPU decoded)", frameCPU);
        int key = cv::waitKey(1);
        if (key == 27 || key == 'q') break;
    }

    std::cout << "Encerrado.\n";
    return 0;
}
