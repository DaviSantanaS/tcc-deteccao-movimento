#pragma once

#include "AvPacket.hpp"

#include <chrono>
#include <memory>

struct EncodedPacket {
    // Historico e evento compartilham o pacote sem modificar seus dados.
    std::shared_ptr<const AVPacket> packet;
    std::chrono::steady_clock::time_point received_at;
    bool has_key_frame = false;
};
