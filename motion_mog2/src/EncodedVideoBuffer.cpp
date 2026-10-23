#include "EncodedVideoBuffer.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

EncodedVideoBuffer::EncodedVideoBuffer(std::size_t max_buffered_packet_count)
    : max_buffered_packet_count_(max_buffered_packet_count) {
    if (this->max_buffered_packet_count_ == 0) {
        throw std::invalid_argument(
            "O limite de pacotes deve ser maior que zero."
        );
    }
}

void EncodedVideoBuffer::appendPackets(std::vector<AvPacketPtr>& pending_encoded_packets) {
    if (this->isFull()) {
        return;
    }

    const std::size_t remaining_packet_capacity =
        this->max_buffered_packet_count_ - this->encoded_packets_buffer_.size();
    const std::size_t packets_to_insert =
        std::min(remaining_packet_capacity, pending_encoded_packets.size());

    this->encoded_packets_buffer_.reserve(this->encoded_packets_buffer_.size() + packets_to_insert);
    for (std::size_t packet_index = 0; packet_index < packets_to_insert; ++packet_index) {
        this->encoded_packets_buffer_.push_back(std::move(pending_encoded_packets[packet_index]));
    }
    pending_encoded_packets.erase(
        pending_encoded_packets.begin(),
        pending_encoded_packets.begin() + static_cast<std::ptrdiff_t>(packets_to_insert)
    );
}

std::vector<AvPacketPtr>
EncodedVideoBuffer::takeBufferedEncodedPackets() {
    std::vector<AvPacketPtr> transferred_encoded_packets;
    transferred_encoded_packets.swap(this->encoded_packets_buffer_);
    return transferred_encoded_packets;
}
