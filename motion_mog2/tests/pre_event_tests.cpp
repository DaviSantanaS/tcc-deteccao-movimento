#include "config/PreEventConfig.hpp"

#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireFailure(const std::function<void()>& operation) {
    bool exception_reported = false;
    try {
        operation();
    } catch (const std::exception&) {
        exception_reported = true;
    }
    require(exception_reported, "A entrada invalida foi aceita.");
}

void testSecondsConversion() {
    require(calculatePreEventFrameCount(0, 59.94) == 0, "Zero segundos.");
    require(calculatePreEventFrameCount(5, 30) == 150, "Cinco segundos a 30 FPS.");
    require(calculatePreEventFrameCount(5, 59.94) == 300, "Arredondamento a 59.94 FPS.");
    require(calculatePreEventFrameCount(2.5, 30) == 75, "Segundos fracionarios.");
    require(calculatePreEventFrameCount(0.1, 30) == 3, "Conversao de 0.1 segundo.");
    require(calculatePreEventFrameCount(0.001, 30) == 1, "Janela menor que um frame.");
    require(parsePreEventSeconds("2.5") == 2.5, "Leitura de segundos fracionarios.");

    for (const std::string pre_event_seconds_text : {"-1", "nan", "inf", "1s", "", "1,5", "1e400"}) {
        requireFailure([&] { parsePreEventSeconds(pre_event_seconds_text); });
    }
    for (double invalid_frames_per_second : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        requireFailure([&] { calculatePreEventFrameCount(5, invalid_frames_per_second); });
    }
    requireFailure([] { calculatePreEventFrameCount(-1, 30); });
    requireFailure([] { calculatePreEventFrameCount(std::ldexp(1.0, 64), 1); });
    requireFailure([] {
        calculatePreEventFrameCount(std::numeric_limits<double>::max(), 60);
    });
}

int main() {
    try {
        testSecondsConversion();
        std::cout << "PASS: conversao e validacao do tempo de pre-evento.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
