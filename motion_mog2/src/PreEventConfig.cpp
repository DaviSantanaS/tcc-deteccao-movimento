#include "PreEventConfig.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

double parsePreEventSeconds(const std::string& pre_event_seconds_text) {
    std::size_t parsed_character_count = 0;
    double pre_event_seconds = 0.0;

    try {
        pre_event_seconds = std::stod(pre_event_seconds_text, &parsed_character_count);
    } catch (const std::exception&) {
        throw std::runtime_error("Tempo de pre-evento invalido: " + pre_event_seconds_text);
    }

    if (parsed_character_count != pre_event_seconds_text.size() ||
        !std::isfinite(pre_event_seconds) || pre_event_seconds < 0.0) {
        throw std::runtime_error(
            "O pre-evento deve ser um numero finito de segundos >= 0: " + pre_event_seconds_text
        );
    }

    return pre_event_seconds;
}

std::uint64_t calculatePreEventFrameCount(double pre_event_seconds, double stream_fps) {
    if (!std::isfinite(pre_event_seconds) || pre_event_seconds < 0.0 ||
        !std::isfinite(stream_fps) || stream_fps <= 0.0) {
        throw std::runtime_error("Tempo de pre-evento ou FPS invalido.");
    }

    const double rounded_frame_count = std::ceil(pre_event_seconds * stream_fps);
    const double frame_count_upper_bound =
        std::ldexp(1.0, std::numeric_limits<std::uint64_t>::digits);

    if (!std::isfinite(rounded_frame_count) ||
        rounded_frame_count >= frame_count_upper_bound) {
        throw std::runtime_error("Pre-evento excede o limite da contagem de frames.");
    }

    if (pre_event_seconds > 0.0 && rounded_frame_count == 0.0) {
        return 1;
    }

    return static_cast<std::uint64_t>(rounded_frame_count);
}
