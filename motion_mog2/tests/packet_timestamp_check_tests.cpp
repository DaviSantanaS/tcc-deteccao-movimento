#include "recording/PacketTimestampCheck.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>

extern "C" {
#include <libavutil/avutil.h>
}

int main() {
    try {
        struct TimestampCase {
            std::int64_t pts;
            std::int64_t dts;
            std::optional<std::int64_t> previous_dts;
            PacketTimestampIssue expected_issue;
        };
        const std::array<TimestampCase, 9> cases{{
            {11, 10, 9, PacketTimestampIssue::none},
            {AV_NOPTS_VALUE, 10, 9, PacketTimestampIssue::missing_timestamp},
            {10, AV_NOPTS_VALUE, 9, PacketTimestampIssue::missing_timestamp},
            {9, 10, 11, PacketTimestampIssue::pts_before_dts},
            {0, 0, 0, PacketTimestampIssue::non_increasing_dts},
            {1, -1, 0, PacketTimestampIssue::non_increasing_dts},
            {5, 5, std::nullopt, PacketTimestampIssue::none},
            {AV_NOPTS_VALUE, AV_NOPTS_VALUE, 99, PacketTimestampIssue::missing_timestamp},
            {-1, -2, -3, PacketTimestampIssue::none},
        }};
        for (const auto& test_case : cases) {
            AVPacket packet{};
            packet.pts = test_case.pts;
            packet.dts = test_case.dts;
            if (checkPacketTimestampsForMuxing(packet, test_case.previous_dts) != test_case.expected_issue) {
                throw std::runtime_error("Classificacao ou prioridade de timestamps incorreta.");
            }
            if (packet.pts != test_case.pts || packet.dts != test_case.dts) {
                throw std::runtime_error("A verificacao nao deve alterar timestamps.");
            }
        }
        std::cout << "PASS: E06-T01 nove casos verificam timestamps, prioridade e ausencia de alteracao dos carimbos.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
