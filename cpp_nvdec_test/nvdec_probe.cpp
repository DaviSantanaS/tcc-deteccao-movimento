#include <iostream>
#include <vector>
#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/core/cuda.hpp>

int main(int argc, char** argv) {
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";

    int devCount = cv::cuda::getCudaEnabledDeviceCount();
    if (devCount <= 0) {
        std::cerr << "Sem GPU CUDA disponível no OpenCV.\n";
        return 1;
    }
    std::cout << "GPUs CUDA visíveis pelo OpenCV: " << devCount << "\n";

    cv::cudacodec::VideoReaderInitParams params;
    params.allowFrameDrop = false;
    params.rawMode = false; // saída já em BGR (conversão feita no próprio cudacodec)

    std::vector<int> sourceParams; // vazio para RTSP/FFmpeg path interno

    try {
        cv::Ptr<cv::cudacodec::VideoReader> reader =
            cv::cudacodec::createVideoReader(url, sourceParams, params);

        cv::cuda::GpuMat d_frame;
        int frames = 0;
        const int maxFrames = 120;

        int64 t0 = cv::getTickCount();
        while (frames < maxFrames) {
            if (!reader->nextFrame(d_frame)) break;
            if (d_frame.empty()) continue;
            frames++;
        }
        double dt = (cv::getTickCount() - t0) / cv::getTickFrequency();
        double fps = (dt > 0) ? frames / dt : 0.0;

        std::cout << "Lidos " << frames << " frames em " << dt << "s  (~" << fps << " FPS)\n";
        if (!d_frame.empty())
            std::cout << "Último frame (GPU): " << d_frame.cols << "x" << d_frame.rows << "\n";

        std::cout << "OK: NVDEC via OpenCV cudacodec funcionando.\n";
    } catch (const cv::Exception& e) {
        std::cerr << "Falha NVDEC: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
