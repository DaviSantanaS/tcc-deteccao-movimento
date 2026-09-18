#pragma once

#include "AvPacket.hpp"

#include <vector>

struct EncodedFramePackets {
    std::vector<AvPacketPtr> encoded_packets;
};
