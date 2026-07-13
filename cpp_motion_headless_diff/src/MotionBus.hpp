#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <cerrno>
class MotionBus{
  const char* path_; int fd_;
public:
  explicit MotionBus(const char* path="/tmp/motion_bus"):path_(path),fd_(-1){}
  static long long now_ms(){ using namespace std::chrono; return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); }
  void ensure_open(){ if(fd_!=-1) return; int fd=open(path_,O_WRONLY|O_NONBLOCK); if(fd!=-1) fd_=fd; }
  void emit(const char* tag,long long ts_ms=-1){
    if(ts_ms<0) ts_ms=now_ms(); ensure_open(); if(fd_==-1) return;
    std::string line=std::string(tag)+" "+std::to_string(ts_ms)+"\n";
    ssize_t n=write(fd_,line.data(),line.size());
    if(n<0 && (errno==EPIPE || errno==ENXIO)){ close(fd_); fd_=-1; }
  }
  void on(){ emit("MOTION_ON"); }  void off(){ emit("MOTION_OFF"); }
  ~MotionBus(){ if(fd_!=-1) close(fd_); }
};
