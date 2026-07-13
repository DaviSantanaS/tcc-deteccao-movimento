#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>   // <-- necessário para std::this_thread::sleep_for

static volatile sig_atomic_t running = 1;
static void handle_sig(int){ running = 0; }

static void ensure_dir(const std::string& path){
    ::mkdir(path.c_str(), 0755); // ignora erro se já existe
}

int main(int argc, char** argv){
    std::string url      = (argc>1)? argv[1] : "rtsp://127.0.0.1:8554/video";
    std::string out_dir  = (argc>2)? argv[2] : std::string(getenv("HOME")?getenv("HOME"):"") + "/tcc/cpp_motion_buffer/dvr_segments";
    std::string seg_time = (argc>3)? argv[3] : "2";

    ensure_dir(out_dir);

    std::signal(SIGINT,  handle_sig);
    std::signal(SIGTERM, handle_sig);

    // comando FFmpeg (sem reencode, 2s por arquivo, nome com strftime)
    std::vector<const char*> cmd = {
        "ffmpeg","-hide_banner","-nostats",
        "-rtsp_transport","tcp",
        "-i", url.c_str(),
        "-an","-map","0:v:0","-c","copy",
        "-f","segment","-segment_time", seg_time.c_str(),
        "-reset_timestamps","1",
        "-strftime","1",
        (out_dir + "/%Y%m%d_%H%M%S.ts").c_str(),
        nullptr
    };

    while (running){
        pid_t pid = ::fork();
        if (pid==0){
            // child → exec ffmpeg
            ::execvp(cmd[0], (char* const*)cmd.data());
            std::perror("execvp");
            std::_Exit(127);
        }
        if (pid<0){
            std::perror("fork");
            return 1;
        }
        // parent → espera ffmpeg
        int status=0;
        if (::waitpid(pid, &status, 0) < 0){
            if (running) std::perror("waitpid");
        }
        if (!running) break;

        // se saiu por erro, tenta reiniciar após um pequeno atraso
        std::cerr << "[segment_recorder] ffmpeg saiu (status=" << status << "). Reiniciando em 1s...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cerr << "[segment_recorder] encerrado.\n";
    return 0;
}
