#include "EncodedVideoBuffer.hpp"
#include "PreEventConfig.hpp"

#include <cmath>
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
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "A entrada invalida foi aceita.");
}

EncodedPacket makePacket(uint64_t index, bool key_frame) {
    EncodedPacket packet;
    packet.data.assign(index % 7 + 1, static_cast<unsigned char>(index % 256));
    packet.has_key_frame = key_frame;
    packet.received_at = std::chrono::steady_clock::time_point{} +
        std::chrono::milliseconds(index * 10);
    return packet;
}

void feed(EncodedVideoBuffer& buffer, uint64_t first, uint64_t last) {
    for (uint64_t index = first; index <= last; ++index) {
        buffer.updatePreEventBuffer({makePacket(index, index % 60 == 0)}, index);
    }
}

size_t expectedBytes(uint64_t first, uint64_t last) {
    size_t bytes = 0;
    for (uint64_t index = first; index <= last; ++index) {
        bytes += index % 7 + 1;
    }
    return bytes;
}

void testSecondsConversion() {
    require(calculatePreEventFrameCount(0, 59.94) == 0, "Zero segundos.");
    require(calculatePreEventFrameCount(5, 30) == 150, "Cinco segundos a 30 FPS.");
    require(calculatePreEventFrameCount(5, 59.94) == 300, "Arredondamento a 59.94 FPS.");
    require(calculatePreEventFrameCount(2.5, 30) == 75, "Segundos fracionarios.");
    require(calculatePreEventFrameCount(0.1, 30) == 3, "Conversao de 0.1 segundo.");
    require(calculatePreEventFrameCount(0.001, 30) == 1, "Janela menor que um frame.");
    require(parsePreEventSeconds("2.5") == 2.5, "Leitura de segundos fracionarios.");

    for (const std::string value : {"-1", "nan", "inf", "1s", "", "1,5", "1e400"}) {
        requireFailure([&] { parsePreEventSeconds(value); });
    }
    for (double fps : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        requireFailure([&] { calculatePreEventFrameCount(5, fps); });
    }
    requireFailure([] { calculatePreEventFrameCount(-1, 30); });
    requireFailure([] { calculatePreEventFrameCount(std::ldexp(1.0, 64), 1); });
    requireFailure([] {
        calculatePreEventFrameCount(std::numeric_limits<double>::max(), 60);
    });
}

void testZeroPreEvent() {
    EncodedVideoBuffer buffer;
    feed(buffer, 0, 100);
    const auto info = buffer.startMotion(100, {makePacket(100, false)});
    require(info.start_decoded_frame_index == 60, "Zero deve usar o GOP corrente.");
    require(info.gop_encoded_packet_count == 41, "GOP corrente incorreto.");
    require(info.requested_pre_event_frame_count == 0, "Padrao deve ser zero.");
    require(info.pre_event_history_sufficient, "GOP corrente com keyframe disponivel.");
}

void testMultipleGops() {
    EncodedVideoBuffer buffer(150);
    feed(buffer, 0, 500);
    const auto info = buffer.startMotion(500, {makePacket(500, false)});
    require(info.start_decoded_frame_index == 300, "Deve selecionar o keyframe 300.");
    require(info.extra_decoded_frames_before_motion == 200, "Margem do GOP incorreta.");
    require(info.gop_encoded_packet_count == 201, "MOTION_ON deve entrar uma unica vez.");
    require(info.requested_pre_event_frame_count == 150, "Pedido deve ser preservado.");
    require(info.pre_event_history_sufficient, "Historico suficiente nao reconhecido.");
    const auto complete = buffer.finishMotion();
    require(complete.encoded_byte_count == expectedBytes(300, 500), "Bytes do historico.");
    require(complete.key_frame_count == 4, "Devem existir quatro keyframes.");
    require(std::abs(complete.duration_seconds - 2.0) < 1e-9, "Extremos temporais do lote.");
}

void testExactKeyframeBoundary() {
    EncodedVideoBuffer buffer(120);
    feed(buffer, 0, 480);
    const auto info = buffer.startMotion(480, {makePacket(480, true)});
    require(info.start_decoded_frame_index == 360, "Keyframe exatamente no limite.");
    require(info.gop_encoded_packet_count == 121, "Limite inclusivo incorreto.");
}

void testInsufficientHistory() {
    EncodedVideoBuffer buffer(150);
    feed(buffer, 0, 30);
    const auto info = buffer.startMotion(30, {makePacket(30, false)});
    require(info.start_decoded_frame_index == 0, "Usar o historico disponivel.");
    require(info.gop_encoded_packet_count == 31, "Historico inicial incorreto.");
    require(!info.pre_event_history_sufficient, "Nao pode prometer 150 frames.");

    EncodedVideoBuffer large_buffer(std::numeric_limits<uint64_t>::max());
    feed(large_buffer, 0, 10);
    const auto large_info = large_buffer.startMotion(10, {makePacket(10, false)});
    require(large_info.start_decoded_frame_index == 0, "Subtracao sem underflow.");
    require(!large_info.pre_event_history_sufficient, "Historico grande indisponivel.");
}

void testMissingAndLateKeyframe() {
    EncodedVideoBuffer buffer(150);
    for (uint64_t index = 0; index < 60; ++index) {
        buffer.updatePreEventBuffer({makePacket(index, false)}, index);
    }
    const auto missing = buffer.startMotion(59, {makePacket(59, false)});
    require(!missing.starts_with_key_frame, "Nao inventar um keyframe.");
    require(!missing.pre_event_history_sufficient, "Sem keyframe, historico insuficiente.");
    require(missing.gop_encoded_packet_count == 1, "Preservar o fallback corrente.");
    buffer.finishMotion();

    feed(buffer, 60, 70);
    const auto late = buffer.startMotion(70, {makePacket(70, false)});
    require(late.start_decoded_frame_index == 60, "Comecar no primeiro keyframe disponivel.");
    require(late.gop_encoded_packet_count == 11, "Descartar prefixo sem keyframe.");
    require(!late.pre_event_history_sufficient, "Primeiro keyframe chegou tarde.");
}

void testEmptyBatchAdvancesWindow() {
    EncodedVideoBuffer buffer(70);
    feed(buffer, 0, 120);
    buffer.updatePreEventBuffer({}, 190);
    const auto info = buffer.startMotion(190, {});
    require(info.start_decoded_frame_index == 120, "Limpar historico com lote vazio.");
    require(info.gop_encoded_packet_count == 1, "Nao criar pacotes para frames observados.");
    require(info.extra_decoded_frames_before_motion == 70, "Usar indices de observacao.");
}

void testSeveralKeyframesInOneBatch() {
    EncodedVideoBuffer buffer(2);
    buffer.updatePreEventBuffer({makePacket(0, true)}, 0);
    buffer.updatePreEventBuffer({makePacket(1, false), makePacket(2, true),
                                 makePacket(3, false), makePacket(4, true),
                                 makePacket(5, false)}, 1);
    buffer.updatePreEventBuffer({makePacket(6, false)}, 2);
    auto info = buffer.startMotion(2, {makePacket(6, false)});
    require(info.start_decoded_frame_index == 0, "Preservar GOP que cobre o inicio.");
    require(info.gop_encoded_packet_count == 7, "Nao confundir pacotes com frames.");
    require(buffer.finishMotion().encoded_byte_count == expectedBytes(0, 6), "Bytes de lotes.");

    buffer.updatePreEventBuffer({}, 3);
    info = buffer.startMotion(3, {});
    require(info.start_decoded_frame_index == 1, "Usar ultimo keyframe elegivel.");
    require(info.gop_encoded_packet_count == 3, "Selecionar o ultimo GOP do mesmo ciclo.");
}

void testHistorySurvivesPreviousEvent() {
    EncodedVideoBuffer buffer(100);
    feed(buffer, 0, 200);
    buffer.startMotion(200, {makePacket(200, false)});
    for (uint64_t index = 201; index <= 230; ++index) {
        const std::vector<EncodedPacket> packets{makePacket(index, false)};
        buffer.updatePreEventBuffer(packets, index);
        buffer.appendMotionPackets(packets);
    }
    const auto complete = buffer.finishMotion();
    require(complete.encoded_packet_count == 171, "Evento deve conter 60..230 uma vez.");
    require(complete.encoded_byte_count == expectedBytes(60, 230), "Bytes do evento ativo.");
    require(buffer.currentMotionInfo().encoded_packet_count == 0, "Limpar apenas o evento.");

    feed(buffer, 231, 231);
    const auto next = buffer.startMotion(231, {makePacket(231, false)});
    require(next.start_decoded_frame_index == 120, "Historico deve sobreviver ao evento.");
    require(next.gop_encoded_packet_count == 112, "Historico do segundo evento incorreto.");
}

void testExpiredGopsAreDiscarded() {
    EncodedVideoBuffer buffer(100);
    feed(buffer, 0, 2000);
    const auto info = buffer.startMotion(2000, {makePacket(2000, false)});
    require(info.start_decoded_frame_index == 1860, "Selecionar GOP recente.");
    require(info.gop_encoded_packet_count == 141, "Nao acumular GOPs expirados.");
}

int main() {
    try {
        testSecondsConversion();
        testZeroPreEvent();
        testMultipleGops();
        testExactKeyframeBoundary();
        testInsufficientHistory();
        testMissingAndLateKeyframe();
        testEmptyBatchAdvancesWindow();
        testSeveralKeyframesInOneBatch();
        testHistorySurvivesPreviousEvent();
        testExpiredGopsAreDiscarded();
        std::cout << "PASS: 10 grupos de testes de pre-evento.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
