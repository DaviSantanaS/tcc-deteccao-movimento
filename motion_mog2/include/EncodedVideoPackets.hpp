#pragma once

#include "AvPacket.hpp"
#include "VideoStreamParameters.hpp"

#include <memory>
#include <vector>

struct EncodedVideoPackets {
    std::vector<AvPacketPtr> encoded_packets;
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters;
};
