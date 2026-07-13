#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using wall_clock = std::chrono::system_clock; // evitar conflito com clock_t da libc

static volatile sig_atomic_t running = 1;
static void handle_sig(int){ running = 0; }

// Formato dos segmentos: %Y%m%d_%H%M%S.ts (ex.: 20250928_193012.ts)
static bool parse_seg_name(const std::string& name, std::tm& out_tm){
    if (name.size() != std::strlen("YYYYmmdd_HHMMSS.ts")) return false;
    if (name[8] != '_' || name.substr(15) != ".ts") return false;
    std::istringstream is(name.substr(0, 15)); // "YYYYmmdd_HHMMSS"
    is >> std::get_time(&out_tm, "%Y%m%d_%H%M%S");
    return !is.fail();
}

static std::time_t tm_to_time_t(std::tm tm){
    tm.tm_isdst = -1; // auto
    return std::mktime(&tm);
}

static std::string time_to_stamp(std::time_t t, const char* fmt){
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

int main(int argc, char** argv){
    std::string seg_dir = (argc>1)? argv[1] : std::string(getenv("HOME")?getenv("HOME"):"") + "/tcc/cpp_motion_buffer/dvr_segments";
    std::string out_dir = (argc>2)? argv[2] : std::string(getenv("HOME")?getenv("HOME"):"") + "/tcc/cpp_motion_buffer/dvr_out";
    int PRE_SEC  = (argc>3)? std::atoi(argv[3]) : 5;
    int POST_SEC = (argc>4)? std::atoi(argv[4]) : 5;
    int SEG_TIME = 2; // deve bater com o segment_recorder

    fs::create_directories(seg_dir);
    fs::create_directories(out_dir);

    std::signal(SIGINT,  handle_sig);
    std::signal(SIGTERM, handle_sig);

    bool in_motion = false;
    std::time_t t_start_wall = 0;

    std::string line;
    while (running && std::getline(std::cin, line)){
        if (line == "MOTION START" && !in_motion){
            in_motion = true;
            t_start_wall = wall_clock::to_time_t(wall_clock::now());
            std::cerr << "[clipper] START @" << time_to_stamp(t_start_wall, "%F %T") << "\n";
        } else if (line == "MOTION END" && in_motion){
            in_motion = false;
            std::time_t t_end_wall = wall_clock::to_time_t(wall_clock::now());
            std::cerr << "[clipper] END   @" << time_to_stamp(t_end_wall, "%F %T") << "\n";

            std::time_t t0 = t_start_wall - PRE_SEC;
            std::time_t t1 = t_end_wall   + POST_SEC;

            // varre diretório e coleta segmentos que intersectam [t0, t1]
            std::vector<std::pair<std::time_t, fs::path>> segs;
            for (auto& p : fs::directory_iterator(seg_dir)){
                if (!p.is_regular_file()) continue;
                auto name = p.path().filename().string();
                std::tm tm{};
                if (!parse_seg_name(name, tm)) continue;
                std::time_t s = tm_to_time_t(tm);
                std::time_t e = s + SEG_TIME;
                if (e > t0 && s < t1){ // overlap
                    segs.emplace_back(s, p.path());
                }
            }
            std::sort(segs.begin(), segs.end(),
                      [](auto& a, auto& b){ return a.first < b.first; });

            if (segs.empty()){
                std::cerr << "[clipper] nenhum segmento cobre a janela.\n";
                continue;
            }

            // lista para concat
            std::string out_name = time_to_stamp(t0, "%Y%m%d_%H%M%S") + "__" + time_to_stamp(t1, "%H%M%S") + ".mp4";
            fs::path out_path = fs::path(out_dir) / out_name;
            fs::path list_path = out_path.string() + ".list.txt";

            {
                std::ofstream ofs(list_path);
                for (auto& it : segs){
                    ofs << "file '" << fs::absolute(it.second).string() << "'\n";
                }
            }

            // ffmpeg concat (sem reencode, +faststart)
            std::vector<const char*> cmd = {
                "ffmpeg","-hide_banner","-nostats",
                "-f","concat","-safe","0",
                "-i", list_path.c_str(),
                "-c","copy","-movflags","+faststart",
                out_path.c_str(),
                nullptr
            };

            pid_t pid = ::fork();
            if (pid==0){
                ::execvp(cmd[0], (char* const*)cmd.data());
                std::perror("execvp");
                std::_Exit(127);
            }
            int status=0;
            if (::waitpid(pid, &status, 0) < 0){
                std::perror("waitpid");
            } else {
                if (status==0){
                    std::cerr << "[clipper] OK → " << out_path << "\n";
                } else {
                    std::cerr << "[clipper] FFmpeg retornou status=" << status << "\n";
                }
            }
            std::error_code ec; fs::remove(list_path, ec);
        }
    }

    return 0;
}
