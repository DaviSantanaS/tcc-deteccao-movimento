#pragma once

#include <chrono>
#include <vector>

struct EncodedPacket {
    std::vector<unsigned char> data;
    std::chrono::steady_clock::time_point received_at;
    bool has_key_frame = false;
};
