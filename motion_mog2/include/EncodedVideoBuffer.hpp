#pragma once

#include "EncodedVideoPackets.hpp"

#include <cstddef>
#include <vector>

class EncodedVideoBuffer {
public:
    explicit EncodedVideoBuffer(std::size_t max_decoded_frame_count = 1000);

private:
    std::size_t max_decoded_frame_count_;
    std::size_t buffered_decoded_frame_count_ = 0;
    std::vector<EncodedVideoPackets> encoded_video_packets_by_read_;
};
