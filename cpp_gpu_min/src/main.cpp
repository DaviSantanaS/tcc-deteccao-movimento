#include <opencv2/opencv.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudafilters.hpp>   // <-- necessário para morfologia CUDA
#include <iostream>
#include <chrono>
#include <string>
#include <iomanip>

int main(int argc, char** argv) {
    // --------- parâmetros ----------
    std::string url = "rtsp://127.0.0.1:8554/video";
    double enterRatio = 0.02;    // 2% dos pixels => movimento
    double learnRate  = -1.0;    // -1 = automático (MOG2)
    bool showMask = true;        // sobrepor máscara no preview
    bool useGray  = true;        // processar em escala de cinza
    int  procW = 960, procH = 540; // resolução de trabalho (GPU)

    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        auto next = [&](){ return (i+1<argc) ? std::string(argv[++i]) : std::string(); };
        if (a=="--url") url = next();
        else if (a=="--enter") enterRatio = std::stod(next());
        else if (a=="--lr") learnRate = std::stod(next());
        else if (a=="--no-mask") showMask = false;
        else if (a=="--color") useGray = false;
        else if (a=="--proc-w") procW = std::stoi(next());
        else if (a=="--proc-h") procH = std::stoi(next());
        else {
            std::cout << "Uso: " << argv[0]
                      << " [--url rtsp://...] [--enter 0.02] [--lr -1]"
                         " [--no-mask] [--color] [--proc-w 960] [--proc-h 540]\n";
            return 0;
        }
    }

    // --------- GPU ----------
    int devs = cv::cuda::getCudaEnabledDeviceCount();
    if (devs <= 0) { std::cerr << "Nenhuma GPU CUDA detectada.\n"; return 1; }
    cv::cuda::setDevice(0);
    std::cout << "OpenCV: " << CV_VERSION << " | CUDA devices: " << devs << "\n";

    // --------- RTSP (decod CPU por enquanto) ----------
    cv::VideoCapture cap(url, cv::CAP_FFMPEG);
    if (!cap.isOpened()) { std::cerr << "Falha ao abrir: " << url << "\n"; return 2; }
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (!(fps > 1.0 && fps < 240.0)) fps = 25.0;
    int inW = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int inH = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    std::cout << "Input: " << inW << "x" << inH << " @" << fps << "fps\n";

    // --------- operadores GPU ----------
    auto mog2 = cv::cuda::createBackgroundSubtractorMOG2(500, 16.0, false);

    cv::cuda::GpuMat d_bgr, d_work, d_gray, d_mask;
    cv::cuda::Stream stream;

    // kernels (CPU) para construir filtros morfológicos CUDA
    cv::Mat kErode = cv::getStructuringElement(cv::MORPH_RECT, {3,3});
    cv::Mat kDil   = cv::getStructuringElement(cv::MORPH_RECT, {5,5});
    // factories CUDA (ficam em <opencv2/cudafilters.hpp>)
    cv::Ptr<cv::cuda::Filter> fErode = cv::cuda::createMorphologyFilter(cv::MORPH_ERODE,  CV_8UC1, kErode);
    cv::Ptr<cv::cuda::Filter> fDil   = cv::cuda::createMorphologyFilter(cv::MORPH_DILATE, CV_8UC1, kDil);

    // host buffers para visualização
    cv::Mat frameCPU, maskCPU, small, vis;
    const int totalPix = procW * procH;

    std::cout << "Iniciando (ESC para sair)...\n";
    auto t_last = std::chrono::steady_clock::now();
    int frames = 0;

    while (true) {
        if (!cap.read(frameCPU)) { std::cerr << "Fim/erro na captura.\n"; break; }

        // upload + pré-process na GPU
        d_bgr.upload(frameCPU, stream);
        cv::cuda::resize(d_bgr, d_work, cv::Size(procW, procH), 0, 0, cv::INTER_LINEAR, stream);

        if (useGray) {
            cv::cuda::cvtColor(d_work, d_gray, cv::COLOR_BGR2GRAY, 0, stream);
            mog2->apply(d_gray, d_mask, learnRate, stream);
        } else {
            mog2->apply(d_work, d_mask, learnRate, stream);
        }

        // morfologia na GPU (via filtros)
        fErode->apply(d_mask, d_mask, stream);
        fDil->apply(d_mask, d_mask, stream);
        cv::cuda::threshold(d_mask, d_mask, 127, 255, cv::THRESH_BINARY, stream);

        // sincroniza para usar no host
        stream.waitForCompletion();

        int nz = cv::cuda::countNonZero(d_mask);
        double ratio = double(nz) / double(totalPix);
        bool motion = (ratio >= enterRatio);

        // visual
        if (showMask) {
            d_mask.download(maskCPU);
            cv::cvtColor(maskCPU, vis, cv::COLOR_GRAY2BGR);
            cv::resize(frameCPU, small, cv::Size(procW, procH));
            cv::addWeighted(small, 0.8, vis, 0.2, 0.0, small);
        } else {
            cv::resize(frameCPU, small, cv::Size(procW, procH));
        }
        std::ostringstream hud;
        hud.setf(std::ios::fixed); hud<<std::setprecision(2)<<(ratio*100.0)<<"%";
        cv::putText(small, hud.str(), {10,30}, cv::FONT_HERSHEY_SIMPLEX, 0.9, {255,255,255}, 2);
        if (motion) {
            cv::putText(small, "MOTION", {10,65}, cv::FONT_HERSHEY_SIMPLEX, 0.9, {0,0,255}, 3);
            cv::rectangle(small, cv::Rect(10,10, small.cols-20, small.rows-20), {0,255,0}, 2);
        }

        cv::imshow("GPU Motion (MOG2)", small);
        int key = cv::waitKey(1);
        if (key==27 || key=='q') break;

        frames++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - t_last).count() >= 1.0) {
            std::cout << "FPS ~ " << frames << (motion ? "  [MOTION]\n" : "  \n");
            frames = 0; t_last = now;
        }
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}
