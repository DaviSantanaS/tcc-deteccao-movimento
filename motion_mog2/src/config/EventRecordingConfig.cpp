#include "config/EventRecordingConfig.hpp"
#include "config/PreEventConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

int parseMotionFrameCount(const std::string& frame_count_text, const std::string& argument_name) {
    if (frame_count_text.empty() ||
        !std::all_of(frame_count_text.begin(), frame_count_text.end(),
            [](unsigned char character) { return character >= '0' && character <= '9'; })) {
        throw std::invalid_argument(argument_name + " deve ser um inteiro positivo.");
    }
    const auto parsed_frame_count = std::stoull(frame_count_text);
    if (parsed_frame_count == 0 ||
        parsed_frame_count > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(
            argument_name + " deve estar entre 1 e " + std::to_string(std::numeric_limits<int>::max()) + "."
        );
    }
    return static_cast<int>(parsed_frame_count);
}

double parseMotionThreshold(const std::string& motion_threshold_text) {
    std::size_t parsed_character_count = 0;
    double motion_threshold_percent = 0.0;
    try {
        motion_threshold_percent = std::stod(motion_threshold_text, &parsed_character_count);
    } catch (const std::exception&) {
        throw std::invalid_argument("Limiar de movimento invalido: " + motion_threshold_text);
    }
    if (parsed_character_count != motion_threshold_text.size()) {
        throw std::invalid_argument("Limiar de movimento invalido: " + motion_threshold_text);
    }
    if (motion_threshold_percent <= 0.0 || motion_threshold_percent > 100.0) {
        throw std::invalid_argument(
            "motion_threshold_percent deve estar no intervalo (0, 100]. Valor: " +
            std::to_string(motion_threshold_percent)
        );
    }
    return motion_threshold_percent;
}

double parsePostEventSeconds(const std::string& post_event_seconds_text) {
    std::size_t parsed_character_count = 0;
    double post_event_seconds = 0.0;
    try {
        post_event_seconds = std::stod(post_event_seconds_text, &parsed_character_count);
    } catch (const std::exception&) {
        throw std::invalid_argument("Tempo de pos_evento_s invalido: " + post_event_seconds_text);
    }
    if (parsed_character_count != post_event_seconds_text.size() ||
        !std::isfinite(post_event_seconds) || post_event_seconds < 0.0) {
        throw std::invalid_argument(
            "O pos_evento_s deve ser um numero finito de segundos >= 0: " + post_event_seconds_text
        );
    }
    return post_event_seconds;
}

}  // namespace

EventRecordingConfig parseEventRecordingArguments(int argument_count, char** argument_values) {
    try {
        EventRecordingConfig recording_config;
        recording_config.input_source =
            argument_count > 1 ? argument_values[1] : "rtsp://127.0.0.1:8554/video";
        recording_config.output_directory = std::filesystem::absolute(
            argument_count > 2 ? argument_values[2] : "./recordings"
        );
        recording_config.pre_event_seconds = parsePreEventSeconds(
            argument_count > 3 ? argument_values[3] : "5"
        );
        recording_config.motion_threshold_percent = parseMotionThreshold(
            argument_count > 4 ? argument_values[4] : "1.0"
        );
        recording_config.motion_start_frame_count = parseMotionFrameCount(
            argument_count > 5 ? argument_values[5] : "2", "inicio_frames"
        );
        recording_config.motion_end_frame_count = parseMotionFrameCount(
            argument_count > 6 ? argument_values[6] : "3", "fim_frames"
        );
        recording_config.post_event_seconds = parsePostEventSeconds(
            argument_count > 7 ? argument_values[7] : "5"
        );
        recording_config.rtsp_transport = argument_count > 8 ? argument_values[8] : "tcp";
        if (recording_config.rtsp_transport != "tcp" && recording_config.rtsp_transport != "udp" &&
            recording_config.rtsp_transport != "auto") {
            throw std::invalid_argument(
                "rtsp_transporte deve ser tcp, udp ou auto: " + recording_config.rtsp_transport
            );
        }
        return recording_config;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& error) {
        throw std::invalid_argument(error.what());
    }
}
