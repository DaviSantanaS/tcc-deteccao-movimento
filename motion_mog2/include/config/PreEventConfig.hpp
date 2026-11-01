#pragma once

#include <cstdint>
#include <string>

double parsePreEventSeconds(const std::string& pre_event_seconds_text);
std::uint64_t calculatePreEventFrameCount(double pre_event_seconds, double stream_fps);
