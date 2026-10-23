#pragma once

#include "AvPacket.hpp"

#include <cstddef>
#include <vector>

class EncodedVideoBuffer {
public:
    explicit EncodedVideoBuffer(std::size_t max_buffered_packet_count = 1000);

    void appendPackets(std::vector<AvPacketPtr>& pending_encoded_packets);

    [[nodiscard]]
    bool isFull() const {
        return this->encoded_packets_buffer_.size() == this->max_buffered_packet_count_;
    }

    [[nodiscard]]
    std::vector<AvPacketPtr> takeBufferedEncodedPackets();

private:
    std::size_t max_buffered_packet_count_;
    std::vector<AvPacketPtr> encoded_packets_buffer_;
};
