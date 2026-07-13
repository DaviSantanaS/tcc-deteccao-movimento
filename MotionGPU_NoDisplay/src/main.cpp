#include <opencv2/opencv.hpp>
#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <cstdlib>   // getenv
#include <string>
#include <vector>

static int   env_int (const char* k, int   defv){ const char* v = std::getenv(k); return v ? std::atoi(v) : defv; }
static float env_float(const char* k, float defv){ const char* v = std::getenv(k); return v ? std::atof(v) : defv; }

int main(int argc, char** argv) {
    // ===== Config =====
    std::string url = (argc > 1) ? argv[1] : "rtsp://127.0.0.1:8554/video";

    // Ajustes via variáveis de ambiente (sem recompilar):
    //   MOTION_MIN_PIXELS (int), MOTION_ALPHA (float), MOTION_BIN_THR (int)
    const int   MIN_PIXELS = env_int ("MOTION_MIN_PIXELS", 5000);   // sensibilidade
    const float ALPHA      = env_float("MOTION_ALPHA"     , 0.01f); // velocidade do MOG2
    const int   BIN_THR    = env_int ("MOTION_BIN_THR"    , 200);   // limiar de binarização
    const int   OPEN_K     = env_int ("MOTION_OPEN_K"     , 3);     // kernel morfológico (3,5,...)

    // ===== GPU check =====
    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "Nenhuma GPU CUDA visível.\n";
        return 1;
    }
    cv::cuda::setDevice(0);

    // ===== NVDEC via cudacodec (RTSP/TCP) =====
    cv::cudacodec::VideoReaderInitParams p;
    p.allowFrameDrop = true;
    p.rawMode        = false;
    p.udpSource      = false;  // seu pipeline usa RTSP sobre TCP
    std::vector<int> sourceParams;

    cv::Ptr<cv::cudacodec::VideoReader> reader;
    try {
        reader = cv::cudacodec::createVideoReader(url, sourceParams, p);
    } catch (const cv::Exception& e) {
        std::cerr << "Falha ao abrir NVDEC: " << e.what() << "\n";
        return 2;
    }

    // ===== Background Subtractor + Morfologia (tudo na GPU) =====
    auto mog2 = cv::cuda::createBackgroundSubtractorMOG2(500, 16.0, false);
    mog2->setDetectShadows(false);

    int k = std::max(1, OPEN_K);
    if (k % 2 == 0) k += 1; // kernel ímpar
    cv::Mat kx = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k));
    auto morphOpen = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, kx);

    cv::cuda::GpuMat d_bgr, d_mask;
    cv::cuda::Stream stream;

    bool inMotion = false;
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    auto tMotionStart = t0;

    auto secsFromStart = [&](clock::time_point t){
        using namespace std::chrono;
        return duration_cast<microseconds>(t - t0).count() / 1e6;
    };

    std::cerr << "Iniciando (headless). URL=" << url
              << "  MIN_PIXELS=" << MIN_PIXELS
              << "  ALPHA=" << ALPHA
              << "  BIN_THR=" << BIN_THR
              << "  OPEN_K=" << k << "\n";

    for (;;) {
        bool ok = false;
        try {
            ok = reader->nextFrame(d_bgr, stream);
        } catch (const cv::Exception& e) {
            std::cerr << "NVDEC nextFrame exception: " << e.what() << "\n";
            break;
        }
        if (!ok) break;

        // FG mask (GPU pipeline)
        mog2->apply(d_bgr, d_mask, ALPHA, stream);
        cv::cuda::threshold(d_mask, d_mask, BIN_THR, 255, cv::THRESH_BINARY, stream);
        morphOpen->apply(d_mask, d_mask, stream);

        // garante conclusão antes de contar pixels
        stream.waitForCompletion();

        int nz = cv::cuda::countNonZero(d_mask);   // continua tudo na GPU
        bool motion = (nz > MIN_PIXELS);

        auto tNow = clock::now();

        if (motion && !inMotion) {
            inMotion = true;
            tMotionStart = tNow;
            std::cout << "MOTION_START t=" << std::fixed << std::setprecision(3)
                      << secsFromStart(tNow) << "s\n";
            std::cout.flush();
        } else if (!motion && inMotion) {
            inMotion = false;
            double tEnd = secsFromStart(tNow);
            double tBeg = secsFromStart(tMotionStart);
            std::cout << "MOTION_END   t=" << std::fixed << std::setprecision(3)
                      << tEnd << "s dur=" << (tEnd - tBeg) << "s\n";
            std::cout.flush();
        }
    }

    if (inMotion) {
        auto tNow = clock::now();
        double tEnd = secsFromStart(tNow);
        double tBeg = secsFromStart(tMotionStart);
        std::cout << "MOTION_END   t=" << std::fixed << std::setprecision(3)
                  << tEnd << "s dur=" << (tEnd - tBeg) << "s\n";
    }

    std::cerr << "Encerrado.\n";
    return 0;
}
