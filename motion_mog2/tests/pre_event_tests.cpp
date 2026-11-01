#include "buffers/EncodedVideoBuffer.hpp"
#include "config/PreEventConfig.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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

EncodedPacket createEncodedPacket(std::uint64_t packet_index, bool key_frame_present) {
    AvPacketPtr av_packet = allocateAvPacket();
    const int packet_byte_count = static_cast<int>(packet_index % 7 + 1);
    if (av_new_packet(av_packet.get(), packet_byte_count) < 0) {
        throw std::runtime_error("Nao foi possivel alocar o payload do teste.");
    }
    std::memset(av_packet->data, static_cast<unsigned char>(packet_index % 256), packet_byte_count);
    av_packet->pts = static_cast<std::int64_t>(packet_index);
    av_packet->dts = static_cast<std::int64_t>(packet_index);
    av_packet->duration = 1;
    av_packet->time_base = AVRational{1, 100};
    av_packet->flags = key_frame_present ? AV_PKT_FLAG_KEY : 0;

    EncodedPacket packet;
    packet.packet = std::shared_ptr<const AVPacket>(std::move(av_packet));
    packet.key_frame_present = key_frame_present;
    packet.reception_time = std::chrono::steady_clock::time_point{} +
        std::chrono::milliseconds(packet_index * 10);
    return packet;
}

void appendPreEventPacketSequence(EncodedVideoBuffer& encoded_video_buffer, std::uint64_t first_packet_index, std::uint64_t last_packet_index) {
    for (std::uint64_t packet_index = first_packet_index; packet_index <= last_packet_index; ++packet_index) {
        encoded_video_buffer.updatePreEventBuffer({createEncodedPacket(packet_index, packet_index % 60 == 0)}, packet_index);
    }
}

std::size_t calculateExpectedByteCount(std::uint64_t first_packet_index, std::uint64_t last_packet_index) {
    std::size_t encoded_byte_count = 0;
    for (std::uint64_t packet_index = first_packet_index; packet_index <= last_packet_index; ++packet_index) {
        encoded_byte_count += packet_index % 7 + 1;
    }
    return encoded_byte_count;
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

void testZeroPreEvent() {
    EncodedVideoBuffer encoded_video_buffer;
    appendPreEventPacketSequence(encoded_video_buffer, 0, 100);
    const auto motion_start_info = encoded_video_buffer.startMotion(100, {createEncodedPacket(100, false)});
    require(motion_start_info.start_decoded_frame_index == 60, "Zero deve usar o GOP corrente.");
    require(motion_start_info.gop_encoded_packet_count == 41, "GOP corrente incorreto.");
    require(motion_start_info.requested_pre_event_frame_count == 0, "Padrao deve ser zero.");
    require(motion_start_info.pre_event_history_sufficient, "GOP corrente com keyframe disponivel.");
}

void testMultipleGops() {
    EncodedVideoBuffer encoded_video_buffer(150);
    appendPreEventPacketSequence(encoded_video_buffer, 0, 500);
    const auto motion_start_info = encoded_video_buffer.startMotion(500, {createEncodedPacket(500, false)});
    require(motion_start_info.start_decoded_frame_index == 300, "Deve selecionar o keyframe 300.");
    require(motion_start_info.extra_decoded_frames_before_motion == 200, "Margem do GOP incorreta.");
    require(motion_start_info.gop_encoded_packet_count == 201, "MOTION_ON deve entrar uma unica vez.");
    require(motion_start_info.requested_pre_event_frame_count == 150, "Pedido deve ser preservado.");
    require(motion_start_info.pre_event_history_sufficient, "Historico suficiente nao reconhecido.");
    const auto completed_motion_info = encoded_video_buffer.finishMotion();
    require(completed_motion_info.encoded_byte_count == calculateExpectedByteCount(300, 500), "Bytes do historico.");
    require(completed_motion_info.key_frame_count == 4, "Devem existir quatro keyframes.");
    require(std::abs(completed_motion_info.duration_seconds - 2.0) < 1e-9, "Extremos temporais do lote.");
}

void testExactKeyframeBoundary() {
    EncodedVideoBuffer encoded_video_buffer(120);
    appendPreEventPacketSequence(encoded_video_buffer, 0, 480);
    const auto motion_start_info = encoded_video_buffer.startMotion(480, {createEncodedPacket(480, true)});
    require(motion_start_info.start_decoded_frame_index == 360, "Keyframe exatamente no limite.");
    require(motion_start_info.gop_encoded_packet_count == 121, "Limite inclusivo incorreto.");
}

void testInsufficientHistory() {
    EncodedVideoBuffer encoded_video_buffer(150);
    appendPreEventPacketSequence(encoded_video_buffer, 0, 30);
    const auto motion_start_info = encoded_video_buffer.startMotion(30, {createEncodedPacket(30, false)});
    require(motion_start_info.start_decoded_frame_index == 0, "Usar o historico disponivel.");
    require(motion_start_info.gop_encoded_packet_count == 31, "Historico inicial incorreto.");
    require(!motion_start_info.pre_event_history_sufficient, "Nao pode prometer 150 frames.");

    EncodedVideoBuffer large_window_buffer(std::numeric_limits<std::uint64_t>::max());
    appendPreEventPacketSequence(large_window_buffer, 0, 10);
    const auto large_window_motion_info = large_window_buffer.startMotion(10, {createEncodedPacket(10, false)});
    require(large_window_motion_info.start_decoded_frame_index == 0, "Subtracao sem underflow.");
    require(!large_window_motion_info.pre_event_history_sufficient, "Historico grande indisponivel.");
}

void testMissingAndLateKeyframe() {
    EncodedVideoBuffer encoded_video_buffer(150);
    for (std::uint64_t packet_index = 0; packet_index < 60; ++packet_index) {
        encoded_video_buffer.updatePreEventBuffer({createEncodedPacket(packet_index, false)}, packet_index);
    }
    const auto missing_keyframe_motion_info = encoded_video_buffer.startMotion(59, {createEncodedPacket(59, false)});
    require(!missing_keyframe_motion_info.starts_with_key_frame, "Nao inventar um keyframe.");
    require(!missing_keyframe_motion_info.pre_event_history_sufficient, "Sem keyframe, historico insuficiente.");
    require(missing_keyframe_motion_info.gop_encoded_packet_count == 1, "Preservar o fallback corrente.");
    encoded_video_buffer.finishMotion();

    appendPreEventPacketSequence(encoded_video_buffer, 60, 70);
    const auto late_keyframe_motion_info = encoded_video_buffer.startMotion(70, {createEncodedPacket(70, false)});
    require(late_keyframe_motion_info.start_decoded_frame_index == 60, "Comecar no primeiro keyframe disponivel.");
    require(late_keyframe_motion_info.gop_encoded_packet_count == 11, "Descartar prefixo sem keyframe.");
    require(!late_keyframe_motion_info.pre_event_history_sufficient, "Primeiro keyframe chegou tarde.");
}

void testEmptyBatchAdvancesWindow() {
    EncodedVideoBuffer encoded_video_buffer(70);
    appendPreEventPacketSequence(encoded_video_buffer, 0, 120);
    encoded_video_buffer.updatePreEventBuffer({}, 190);
    const auto motion_start_info = encoded_video_buffer.startMotion(190, {});
    require(motion_start_info.start_decoded_frame_index == 120, "Limpar historico com lote vazio.");
    require(motion_start_info.gop_encoded_packet_count == 1, "Nao criar pacotes para frames observados.");
    require(motion_start_info.extra_decoded_frames_before_motion == 70, "Usar indices de observacao.");
}

void testSeveralKeyframesInOneBatch() {
    EncodedVideoBuffer encoded_video_buffer(2);
    encoded_video_buffer.updatePreEventBuffer({createEncodedPacket(0, true)}, 0);
    encoded_video_buffer.updatePreEventBuffer({createEncodedPacket(1, false), createEncodedPacket(2, true),
                                 createEncodedPacket(3, false), createEncodedPacket(4, true),
                                 createEncodedPacket(5, false)}, 1);
    encoded_video_buffer.updatePreEventBuffer({createEncodedPacket(6, false)}, 2);
    auto motion_start_info = encoded_video_buffer.startMotion(2, {createEncodedPacket(6, false)});
    require(motion_start_info.start_decoded_frame_index == 0, "Preservar GOP que cobre o inicio.");
    require(motion_start_info.gop_encoded_packet_count == 7, "Nao confundir pacotes com frames.");
    require(encoded_video_buffer.finishMotion().encoded_byte_count == calculateExpectedByteCount(0, 6), "Bytes de lotes.");

    encoded_video_buffer.updatePreEventBuffer({}, 3);
    motion_start_info = encoded_video_buffer.startMotion(3, {});
    require(motion_start_info.start_decoded_frame_index == 1, "Usar ultimo keyframe elegivel.");
    require(motion_start_info.gop_encoded_packet_count == 3, "Selecionar o ultimo GOP do mesmo ciclo.");
}

void testHistorySurvivesPreviousEvent() {
    EncodedVideoBuffer encoded_video_buffer(100);
    appendPreEventPacketSequence(encoded_video_buffer, 0, 200);
    encoded_video_buffer.startMotion(200, {createEncodedPacket(200, false)});
    for (std::uint64_t packet_index = 201; packet_index <= 230; ++packet_index) {
        const std::vector<EncodedPacket> packets{createEncodedPacket(packet_index, false)};
        encoded_video_buffer.updatePreEventBuffer(packets, packet_index);
        encoded_video_buffer.appendMotionPackets(packets);
    }
    const auto completed_motion_info = encoded_video_buffer.finishMotion();
    require(completed_motion_info.encoded_packet_count == 171, "Evento deve conter 60..230 uma vez.");
    require(completed_motion_info.encoded_byte_count == calculateExpectedByteCount(60, 230), "Bytes do evento ativo.");
    require(encoded_video_buffer.currentMotionInfo().encoded_packet_count == 171, "Preservar o evento finalizado.");
    require(encoded_video_buffer.currentMotionInfo().encoded_byte_count == calculateExpectedByteCount(60, 230),
            "Preservar os bytes do evento finalizado.");

    appendPreEventPacketSequence(encoded_video_buffer, 231, 231);
    const auto next_motion_info = encoded_video_buffer.startMotion(231, {createEncodedPacket(231, false)});
    require(next_motion_info.start_decoded_frame_index == 120, "Historico deve sobreviver ao evento.");
    require(next_motion_info.gop_encoded_packet_count == 112, "Historico do segundo evento incorreto.");
    require(encoded_video_buffer.currentMotionInfo().encoded_packet_count == 112,
            "Substituir o evento finalizado somente no proximo inicio.");
}

void testExpiredGopsAreDiscarded() {
    EncodedVideoBuffer encoded_video_buffer(100);
    appendPreEventPacketSequence(encoded_video_buffer, 0, 2000);
    const auto motion_start_info = encoded_video_buffer.startMotion(2000, {createEncodedPacket(2000, false)});
    require(motion_start_info.start_decoded_frame_index == 1860, "Selecionar GOP recente.");
    require(motion_start_info.gop_encoded_packet_count == 141, "Nao acumular GOPs expirados.");
}

void testSharedPacketOwnership() {
    EncodedPacket original_encoded_packet = createEncodedPacket(42, true);
    EncodedPacket preserved_encoded_packet = original_encoded_packet;
    std::weak_ptr<const AVPacket> original_packet = original_encoded_packet.packet;
    original_encoded_packet.packet.reset();
    require(preserved_encoded_packet.packet->data[0] == 42, "Payload deve sobreviver a liberacao da origem.");
    require(preserved_encoded_packet.packet->pts == 42 && preserved_encoded_packet.packet->dts == 42 &&
                preserved_encoded_packet.packet->duration == 1,
            "Preservar timestamps e duracao do AVPacket.");
    require(preserved_encoded_packet.packet->time_base.num == 1 && preserved_encoded_packet.packet->time_base.den == 100,
            "Preservar a base de tempo.");
    require((preserved_encoded_packet.packet->flags & AV_PKT_FLAG_KEY) != 0, "Preservar a flag de keyframe.");
    preserved_encoded_packet.packet.reset();
    require(original_packet.expired(), "Liberar o pacote depois da ultima referencia.");

    EncodedVideoBuffer encoded_video_buffer;
    EncodedPacket first_event_packet = createEncodedPacket(0, true);
    std::weak_ptr<const AVPacket> retained_packet = first_event_packet.packet;
    encoded_video_buffer.startMotion(0, {first_event_packet});
    first_event_packet.packet.reset();
    encoded_video_buffer.finishMotion();
    require(!retained_packet.expired(), "MOTION_OFF nao deve liberar o ultimo evento.");
    encoded_video_buffer.startMotion(1, {createEncodedPacket(1, true)});
    require(retained_packet.expired(), "O proximo inicio deve liberar o evento anterior.");
}

void testInvalidPacketsAreRejected() {
    EncodedVideoBuffer encoded_video_buffer;
    requireFailure([&] { encoded_video_buffer.updatePreEventBuffer({EncodedPacket{}}, 0); });
    requireFailure([&] { encoded_video_buffer.appendMotionPackets({EncodedPacket{}}); });
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
        testSharedPacketOwnership();
        testInvalidPacketsAreRejected();
        std::cout << "PASS: 12 grupos de testes de pre-evento e ownership.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
