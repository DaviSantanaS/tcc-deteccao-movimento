#include "AvPacket.hpp"

#include <cassert>

extern "C" {
#include <libavcodec/packet.h>
}

int main() {
    AvPacketPtr packet = allocateAvPacket();
    assert(packet != nullptr);

    return 0;
}
