#include "EncodedVideoBuffer.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

int main() {
    try {
        EncodedVideoBuffer default_buffer;
        EncodedVideoBuffer single_frame_buffer(1);
        EncodedVideoBuffer three_frame_buffer(3);
        EncodedVideoBuffer maximum_frame_limit_buffer(
            std::numeric_limits<std::size_t>::max()
        );

        bool rejected_zero_frame_limit = false;
        try {
            EncodedVideoBuffer invalid_buffer(0);
        } catch (const std::invalid_argument&) {
            rejected_zero_frame_limit = true;
        }

        if (!rejected_zero_frame_limit) {
            throw std::runtime_error("O construtor deve rejeitar limite zero de frames decodificados.");
        }

        std::cout << "PASS: construtor aceita limites positivos de frames decodificados e rejeita zero.\n";

    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
