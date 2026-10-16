#include "EncodedVideoBuffer.hpp"

#include <stdexcept>
#include <utility>

EncodedVideoBuffer::EncodedVideoBuffer(std::size_t max_decoded_frame_count)
    : max_decoded_frame_count_(max_decoded_frame_count) {
    if (max_decoded_frame_count_ == 0) {
        throw std::invalid_argument(
            "O limite de frames decodificados deve ser maior que zero."
        );
    }
}

bool EncodedVideoBuffer::bufferPacketsFromRead(EncodedVideoPackets&& encoded_video_packets) {
    if (buffered_decoded_frame_count_ >= max_decoded_frame_count_) {
        throw std::length_error("O buffer atingiu o limite de frames decodificados.");
    }

    encoded_packets_buffer_.push_back(std::move(encoded_video_packets));
    ++buffered_decoded_frame_count_;
    return buffered_decoded_frame_count_ == max_decoded_frame_count_;
}

std::vector<EncodedVideoPackets>
EncodedVideoBuffer::takeBufferedReadPackets() {
    std::vector<EncodedVideoPackets> transferred_read_packets;
    transferred_read_packets.swap(encoded_packets_buffer_);
    buffered_decoded_frame_count_ = 0;
    return transferred_read_packets;
}
