#pragma once

#include "ffmpeg/AvPacket.hpp"
#include "ffmpeg/VideoStreamParameters.hpp"

#include <memory>
#include <vector>

struct EncodedVideoPackets {
    std::vector<AvPacketPtr> encoded_packets;
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters;
};
