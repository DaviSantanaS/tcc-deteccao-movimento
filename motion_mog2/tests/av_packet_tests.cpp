#include "AvPacket.hpp"

#include <cassert>

extern "C" {
#include <libavcodec/packet.h>
}

int main() {
    AvPacketPtr packet = allocateAvPacket();
    assert(packet != nullptr);

    const int allocation_result = av_new_packet(packet.get(), 1);
    assert(allocation_result == 0);

    packet->data[0] = 42;
    packet->pts = 123;

    AvPacketPtr cloned_packet = cloneAvPacket(*packet);
    assert(cloned_packet != nullptr);
    assert(cloned_packet.get() != packet.get());
    assert(cloned_packet->size == 1);
    assert(cloned_packet->data[0] == 42);
    assert(cloned_packet->pts == 123);

    packet.reset();
    assert(packet == nullptr);
    assert(cloned_packet->size == 1);
    assert(cloned_packet->data[0] == 42);
    assert(cloned_packet->pts == 123);

    return 0;
}
