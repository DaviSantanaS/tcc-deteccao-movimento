#include "EncodedVideoBuffer.hpp"

#include <stdexcept>
#include <utility>

EncodedVideoBuffer::EncodedVideoBuffer(std::size_t max_buffered_read_count)
    : max_buffered_read_count_(max_buffered_read_count) {
    if (max_buffered_read_count_ == 0) {
        throw std::invalid_argument(
            "O limite de leituras deve ser maior que zero."
        );
    }
}
//verificar essas chamadas de função
bool EncodedVideoBuffer::bufferEncodedPackets(std::vector<AvPacketPtr>&& encoded_packets_to_buffer) {
    if (buffered_read_count_ >= max_buffered_read_count_) {
        throw std::length_error("O buffer atingiu o limite de leituras.");
    }

    encoded_packets_buffer_.reserve(encoded_packets_buffer_.size() + encoded_packets_to_buffer.size());
    for (auto& encoded_packet : encoded_packets_to_buffer) {
        encoded_packets_buffer_.push_back(std::move(encoded_packet));
    }
    encoded_packets_to_buffer.clear();
    ++buffered_read_count_;
    return buffered_read_count_ == max_buffered_read_count_;
}

std::vector<AvPacketPtr>
EncodedVideoBuffer::takeBufferedEncodedPackets() {
    std::vector<AvPacketPtr> transferred_encoded_packets;
    transferred_encoded_packets.swap(encoded_packets_buffer_);
    buffered_read_count_ = 0;
    return transferred_encoded_packets;
}
