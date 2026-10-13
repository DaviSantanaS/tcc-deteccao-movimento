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

bool EncodedVideoBuffer::appendReadPackets(EncodedVideoPackets&& encoded_video_packets) {
    if (buffered_decoded_frame_count_ >= max_decoded_frame_count_) {
        throw std::length_error("O buffer atingiu o limite de frames decodificados.");
    }

    encoded_video_packets_by_read_.push_back(std::move(encoded_video_packets));
    ++buffered_decoded_frame_count_;
    return buffered_decoded_frame_count_ == max_decoded_frame_count_;
}
