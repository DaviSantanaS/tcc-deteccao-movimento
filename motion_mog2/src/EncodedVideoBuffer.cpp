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
