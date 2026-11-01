#pragma once

#include "ffmpeg/AvPacket.hpp"

#include <chrono>
#include <memory>

struct EncodedPacket {
    std::shared_ptr<const AVPacket> packet;
    std::chrono::steady_clock::time_point reception_time;
    bool key_frame_present = false;
};
