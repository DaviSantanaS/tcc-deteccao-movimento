#include "config/EventRecordingConfig.hpp"

#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

EventRecordingConfig parseArguments(std::initializer_list<std::string> arguments) {
    std::vector<std::string> argument_storage(arguments);
    std::vector<char*> argument_values;
    for (std::string& argument : argument_storage) {
        argument_values.push_back(argument.data());
    }
    return parseEventRecordingArguments(static_cast<int>(argument_values.size()), argument_values.data());
}

void requireInvalidArguments(
    std::initializer_list<std::string> arguments,
    const std::string& expected_message
) {
    try {
        (void)parseArguments(arguments);
    } catch (const std::invalid_argument& error) {
        require(error.what() == expected_message, "Mensagem inesperada: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Argumentos invalidos foram aceitos.");
}

}  // namespace

int main() {
    try {
        {
            const EventRecordingConfig config = parseArguments({"motion_mog2"});
            require(config.input_source == "rtsp://127.0.0.1:8554/video", "Fonte padrao incorreta.");
            require(config.output_directory == std::filesystem::absolute("./recordings"),
                    "Diretorio padrao incorreto.");
            require(config.pre_event_seconds == 5 && config.post_event_seconds == 5,
                    "Tempos padrao incorretos.");
            require(config.motion_threshold_percent == 1.0 && config.motion_start_frame_count == 2 &&
                        config.motion_end_frame_count == 3, "Parametros padrao do detector incorretos.");
        }
        std::cout << "PASS: argumentos padrao.\n";

        {
            const EventRecordingConfig config = parseArguments(
                {"motion_mog2", "video.mp4", "/tmp/event-recording-config", "2.5", "3.5", "4", "6", "1.25"}
            );
            require(config.input_source == "video.mp4" && config.output_directory == "/tmp/event-recording-config",
                    "Fonte ou diretorio explicito incorreto.");
            require(config.pre_event_seconds == 2.5 && config.post_event_seconds == 1.25 &&
                        config.motion_threshold_percent == 3.5 && config.motion_start_frame_count == 4 &&
                        config.motion_end_frame_count == 6, "Valores explicitos incorretos.");
        }
        std::cout << "PASS: sete argumentos validos.\n";

        requireInvalidArguments(
            {"motion_mog2", "video.mp4", "/tmp/x", "5", "1.0", "2", "3", "-1"},
            "O pos_evento_s deve ser um numero finito de segundos >= 0: -1"
        );
        std::cout << "PASS: pos_evento_s negativo rejeitado.\n";

        requireInvalidArguments(
            {"motion_mog2", "video.mp4", "/tmp/x", "5", "1.0", "2", "3", "abc"},
            "Tempo de pos_evento_s invalido: abc"
        );
        std::cout << "PASS: pos_evento_s nao numerico rejeitado.\n";

        for (const std::string threshold_text : {"abc", "1.0abc"}) {
            requireInvalidArguments(
                {"motion_mog2", "video.mp4", "/tmp/x", "5", threshold_text, "2", "3"},
                "Limiar de movimento invalido: " + threshold_text
            );
        }
        std::cout << "PASS: limiar nao numerico ou parcial usa a mensagem do projeto.\n";

        requireInvalidArguments(
            {"motion_mog2", "video.mp4", "/tmp/x", "5", "1.0", "0", "3"},
            "inicio_frames deve estar entre 1 e " + std::to_string(std::numeric_limits<int>::max()) + "."
        );
        std::cout << "PASS: inicio_frames zero rejeitado.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
