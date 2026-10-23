#pragma once

#include "AvPacket.hpp"

#include <cstddef>
#include <vector>

class EncodedVideoBuffer {
public:
    explicit EncodedVideoBuffer(std::size_t max_buffered_read_count = 1000);

    bool bufferEncodedPackets(std::vector<AvPacketPtr>&& encoded_packets_to_buffer);

    [[nodiscard]]
    std::vector<AvPacketPtr> takeBufferedEncodedPackets();

private:
    std::size_t max_buffered_read_count_;
    std::size_t buffered_read_count_ = 0;
    std::vector<AvPacketPtr> encoded_packets_buffer_;
};
