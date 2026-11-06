#include "buffers/EncodedVideoBuffer.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

void EncodedVideoBuffer::pushPacket(AvPacketPtr encoded_packet) {
    if (!encoded_packet) {
        throw std::invalid_argument("O buffer nao aceita AVPacket nulo.");
    }

    std::lock_guard<std::mutex> lock(this->encoded_packets_mutex_);
    this->encoded_packets_.push_back(std::move(encoded_packet));
}

AvPacketPtr EncodedVideoBuffer::tryPopPacket() {
    std::lock_guard<std::mutex> lock(this->encoded_packets_mutex_);
    if (this->encoded_packets_.empty()) {
        return nullptr;
    }

    AvPacketPtr oldest_encoded_packet = std::move(this->encoded_packets_.front());
    this->encoded_packets_.pop_front();
    return oldest_encoded_packet;
}

std::size_t EncodedVideoBuffer::getPacketCount() const {
    std::lock_guard<std::mutex> lock(this->encoded_packets_mutex_);
    return this->encoded_packets_.size();
}
