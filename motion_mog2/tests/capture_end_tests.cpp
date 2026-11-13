#include "app/CaptureEnd.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/error.h>
}

namespace {

void requireEnd(PacketReadEnd packet_read_end, bool source_is_live, bool interrupted_by_signal,
                CaptureEndReason expected_reason, int expected_code, const char* expected_name) {
    const CaptureEndReason reason = classifyCaptureEnd(packet_read_end, source_is_live, interrupted_by_signal);
    if (reason != expected_reason || getExitCodeForCaptureEnd(reason) != expected_code ||
        std::strcmp(getCaptureEndReasonName(reason), expected_name) != 0) {
        throw std::runtime_error("Motivo, codigo ou nome do fim da captura incorreto.");
    }
}

}

int main() {
    try {
        constexpr std::array<PacketReadEndReason, 6> reasons{
            PacketReadEndReason::not_ended, PacketReadEndReason::end_of_stream,
            PacketReadEndReason::interrupted, PacketReadEndReason::read_error,
            PacketReadEndReason::packet_preparation_error, PacketReadEndReason::decoder_stalled
        };
        for (const auto reason : reasons) {
            for (const bool source_is_live : {false, true}) {
                requireEnd({reason, AVERROR_EOF}, source_is_live, true, CaptureEndReason::interrupted, 0, "interrupted");
            }
        }
        requireEnd({PacketReadEndReason::interrupted, AVERROR_EXIT}, true, false,
                   CaptureEndReason::interrupted, 0, "interrupted");
        std::cout << "PASS: E07-T01 sinal prevalece sobre todos os motivos; interrupcao retorna 0.\n";
        requireEnd({PacketReadEndReason::end_of_stream, AVERROR_EOF}, false, false,
                   CaptureEndReason::file_end, 0, "file_end");
        std::cout << "PASS: E07-T02 fim de arquivo retorna file_end e codigo 0.\n";
        requireEnd({PacketReadEndReason::end_of_stream, AVERROR_EOF}, true, false,
                   CaptureEndReason::live_source_closed, 3, "live_source_closed");
        std::cout << "PASS: E07-T03 fim de fonte ao vivo retorna live_source_closed e codigo 3.\n";
        requireEnd({PacketReadEndReason::read_error, -5}, true, false,
                   CaptureEndReason::read_error, 4, "read_error");
        std::cout << "PASS: E07-T04 erro de leitura retorna read_error e codigo 4.\n";
        requireEnd({PacketReadEndReason::packet_preparation_error, 0}, false, false,
                   CaptureEndReason::packet_error, 5, "packet_error");
        std::cout << "PASS: E07-T05 erro de preparo retorna packet_error e codigo 5.\n";
        requireEnd({}, false, false, CaptureEndReason::read_error, 4, "read_error");
        std::cout << "PASS: E07-T06 fim sem motivo retorna read_error e codigo 4.\n";
        for (const bool source_is_live : {false, true}) {
            requireEnd({PacketReadEndReason::decoder_stalled, 0}, source_is_live, false,
                       CaptureEndReason::decoder_stalled, 6, "decoder_stalled");
        }
        std::cout << "PASS: E09-T03 decoder_stalled retorna codigo 6; sinal continua prevalecendo.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
