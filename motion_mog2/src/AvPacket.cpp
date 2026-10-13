#include "AvPacket.hpp"

#include <new>
#include <stdexcept>

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

void prepareEncodedPacketForOutput(
    AVPacket& encoded_packet,
    AVRational source_time_base,
    AVRational output_time_base,
    int output_stream_index
) {
    if (source_time_base.num <= 0 || source_time_base.den <= 0 ||
        output_time_base.num <= 0 || output_time_base.den <= 0 ||
        output_stream_index < 0) {
        throw std::invalid_argument(
            "As bases de tempo devem ser positivas e o indice da stream nao negativo."
        );
    }

    av_packet_rescale_ts(
        &encoded_packet, source_time_base, output_time_base
    );
    encoded_packet.time_base = output_time_base;
    encoded_packet.stream_index = output_stream_index;
    encoded_packet.pos = -1;
}
