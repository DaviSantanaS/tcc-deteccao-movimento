#pragma once

#include <filesystem>
#include <string>

struct EventRecordingConfig {
    std::string input_source;
    std::filesystem::path output_directory;
    double pre_event_seconds = 5;
    double post_event_seconds = 5;
    double motion_threshold_percent = 1.0;
    int motion_start_frame_count = 2;
    int motion_end_frame_count = 3;
};

EventRecordingConfig parseEventRecordingArguments(int argument_count, char** argument_values);
