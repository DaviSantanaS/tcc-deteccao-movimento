#pragma once

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

inline void printEncodedBytesPrefix(
    std::string_view label,
    const unsigned char* encoded_bytes,
    std::size_t byte_count
) {
    if (encoded_bytes == nullptr && byte_count != 0) {
        throw std::invalid_argument("Dados codificados nulos com quantidade de bytes positiva.");
    }

    const auto original_flags = std::cout.flags();
    const auto original_fill = std::cout.fill();
    const auto original_width = std::cout.width();
    std::cout.width(0);
    std::cout << label << ": bytes=" << std::dec << std::noshowbase << byte_count << " prefix=";

    constexpr std::size_t maximum_prefix_byte_count = 8;
    const auto prefix_byte_count = byte_count < maximum_prefix_byte_count
        ? byte_count : maximum_prefix_byte_count;
    if (prefix_byte_count == 0) {
        std::cout << "empty";
    }
    for (std::size_t byte_index = 0; byte_index < prefix_byte_count; ++byte_index) {
        if (byte_index != 0) {
            std::cout << ' ';
        }
        std::cout << std::hex << std::nouppercase << std::setfill('0') << std::setw(2)
                  << static_cast<unsigned int>(encoded_bytes[byte_index]);
    }
    std::cout << '\n';
    std::cout.flags(original_flags);
    std::cout.fill(original_fill);
    std::cout.width(original_width);
}
