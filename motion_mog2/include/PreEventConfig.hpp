#pragma once

#include <cstdint>
#include <string>

double parsePreEventSeconds(const std::string& value);
uint64_t calculatePreEventFrameCount(double pre_event_seconds, double stream_fps);
