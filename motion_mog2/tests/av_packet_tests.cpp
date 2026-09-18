#include "AvPacket.hpp"
#include "EncodedVideoBuffer.hpp"

extern "C" {
#include <libavutil/rational.h>
}

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AvPacketPtr makePacket(
    int byte_size,
    int64_t pts,
    int64_t duration,
    bool key_frame
) {
    AvPacketPtr packet = allocateAvPacket();
    const int result = av_new_packet(packet.get(), byte_size);
    require(result == 0, "av_new_packet falhou");

    packet->pts = pts;
    packet->dts = pts;
    packet->duration = duration;
    packet->time_base = AVRational{1, 1000};
    packet->flags = key_frame ? AV_PKT_FLAG_KEY : 0;

    for (int index = 0; index < byte_size; ++index) {
        packet->data[index] = static_cast<uint8_t>(index % 251);
    }

    return packet;
}

void testCloneKeepsPacketDataAlive() {
    AvPacketPtr original = makePacket(32, 0, 40, true);
    const uint8_t* shared_data_address = original->data;

    AvPacketPtr clone = cloneAvPacket(*original);
    require(
        clone->data == shared_data_address,
        "av_packet_clone copiou o payload em vez de referencia-lo"
    );

    original.reset();
    require(clone->size == 32, "o clone perdeu o tamanho do pacote");
    require(clone->data[7] == 7, "o clone perdeu o payload do pacote");
}

void testEncodedVideoBufferClonesAvPackets() {
    EncodedVideoBuffer buffer;
    std::vector<AvPacketPtr> packets;
    packets.push_back(makePacket(100, 0, 40, true));
    packets.push_back(makePacket(60, 40, 40, false));

    buffer.updateCurrentGop(packets, 10);
    const MotionBufferStartInfo start_info = buffer.startMotion(11, packets);

    require(start_info.starts_with_key_frame, "o buffer nao inicia no keyframe");
    require(
        start_info.gop_encoded_packet_count == 2,
        "o buffer nao preservou os dois pacotes do GOP"
    );
    require(
        start_info.start_decoded_frame_index == 10,
        "o indice inicial do buffer esta incorreto"
    );

    packets.clear();

    std::vector<AvPacketPtr> next_packets;
    next_packets.push_back(makePacket(80, 80, 40, false));
    buffer.appendMotionPackets(next_packets);

    const MotionBufferCompleteInfo complete_info = buffer.finishMotion();
    require(
        complete_info.encoded_packet_count == 3,
        "o buffer nao preservou os tres pacotes"
    );
    require(
        complete_info.encoded_byte_count == 240,
        "a soma de bytes do buffer esta incorreta"
    );
    require(complete_info.key_frame_count == 1, "a contagem de keyframes falhou");
    require(complete_info.starts_with_key_frame, "o primeiro pacote nao e keyframe");
    require(
        complete_info.duration_seconds > 0.119 &&
            complete_info.duration_seconds < 0.121,
        "a duracao calculada pelos timestamps esta incorreta"
    );
}

}  // namespace

int main() {
    try {
        testCloneKeepsPacketDataAlive();
        testEncodedVideoBufferClonesAvPackets();
    } catch (const std::exception& error) {
        std::cerr << "av_packet_tests: falhou: " << error.what() << "\n";
        return 1;
    }

    std::cout << "av_packet_tests: ok\n";
    return 0;
}
