#pragma once

extern "C" {
#include <libavcodec/packet.h>
}

#include <memory>
#include <stdexcept>

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        av_packet_free(&packet);
    }
};

using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

inline AvPacketPtr allocateAvPacket() {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        throw std::runtime_error("Nao foi possivel alocar AVPacket.");
    }

    return AvPacketPtr(packet);
}

inline AvPacketPtr cloneAvPacket(const AVPacket& source) {
    AVPacket* clone = av_packet_clone(&source);
    if (clone == nullptr) {
        throw std::runtime_error("Nao foi possivel clonar AVPacket.");
    }

    return AvPacketPtr(clone);
}
