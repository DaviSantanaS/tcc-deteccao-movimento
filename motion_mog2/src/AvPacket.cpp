#include "AvPacket.hpp"

#include <new>

void AvPacketDeleter::operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
}

AvPacketPtr allocateAvPacket() {
    AvPacketPtr packet{av_packet_alloc()};

    if (!packet) {
        throw std::bad_alloc{};
    }

    return packet;
}

AvPacketPtr cloneAvPacket(const AVPacket& source) {
    AvPacketPtr packet{av_packet_clone(&source)};

    if (!packet) {
        throw std::bad_alloc{};
    }

    return packet;
}
