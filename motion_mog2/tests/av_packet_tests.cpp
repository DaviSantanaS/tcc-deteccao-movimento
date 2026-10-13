#include "AvPacket.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
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

    AVPacket* const original_packet = cloned_packet.get();
    const uint8_t* const original_data = cloned_packet->data;
    const AVBufferRef* const original_buffer = cloned_packet->buf;
    cloned_packet->pts = 90000;
    cloned_packet->dts = 81000;
    cloned_packet->duration = 3000;
    cloned_packet->time_base = {1, 90000};
    cloned_packet->stream_index = 5;
    cloned_packet->pos = 12345;
    cloned_packet->flags = AV_PKT_FLAG_KEY;

    prepareEncodedPacketForOutput(*cloned_packet, {1, 90000}, {1, 1000}, 0);

    assert(cloned_packet->pts == 1000);
    assert(cloned_packet->dts == 900);
    assert(cloned_packet->duration == 33);
    assert(cloned_packet->time_base.num == 1 && cloned_packet->time_base.den == 1000);
    assert(cloned_packet->stream_index == 0);
    assert(cloned_packet->pos == -1);
    assert(cloned_packet.get() == original_packet);
    assert(cloned_packet->data == original_data);
    assert(cloned_packet->buf == original_buffer);
    assert(cloned_packet->size == 1 && cloned_packet->data[0] == 42);
    assert(cloned_packet->flags == AV_PKT_FLAG_KEY);

    prepareEncodedPacketForOutput(*cloned_packet, {1, 1000}, {1, 1000}, 2);
    assert(cloned_packet->pts == 1000);
    assert(cloned_packet->dts == 900);
    assert(cloned_packet->duration == 33);
    assert(cloned_packet->stream_index == 2);

    cloned_packet->pts = -9000;
    cloned_packet->dts = -18000;
    cloned_packet->duration = 3000;
    cloned_packet->time_base = {1, 90000};
    prepareEncodedPacketForOutput(*cloned_packet, {1, 90000}, {1, 1000}, 0);
    assert(cloned_packet->pts == -100);
    assert(cloned_packet->dts == -200);
    assert(cloned_packet->duration == 33);

    cloned_packet->pts = AV_NOPTS_VALUE;
    cloned_packet->dts = 90000;
    cloned_packet->duration = 0;
    cloned_packet->time_base = {1, 90000};
    prepareEncodedPacketForOutput(*cloned_packet, {1, 90000}, {1, 1000}, 0);
    assert(cloned_packet->pts == AV_NOPTS_VALUE);
    assert(cloned_packet->dts == 1000);
    assert(cloned_packet->duration == 0);

    cloned_packet->pts = 90000;
    cloned_packet->dts = AV_NOPTS_VALUE;
    cloned_packet->time_base = {1, 90000};
    prepareEncodedPacketForOutput(*cloned_packet, {1, 90000}, {1, 1000}, 0);
    assert(cloned_packet->pts == 1000);
    assert(cloned_packet->dts == AV_NOPTS_VALUE);
    assert(cloned_packet->duration == 0);

    cloned_packet->pts = AV_NOPTS_VALUE;
    cloned_packet->time_base = {1, 90000};
    prepareEncodedPacketForOutput(*cloned_packet, {1, 90000}, {1, 1000}, 0);
    assert(cloned_packet->pts == AV_NOPTS_VALUE);
    assert(cloned_packet->dts == AV_NOPTS_VALUE);
    assert(cloned_packet->duration == 0);

    cloned_packet->pts = 90000;
    cloned_packet->dts = 81000;
    cloned_packet->duration = 3000;
    cloned_packet->time_base = {1, 90000};
    cloned_packet->stream_index = 5;
    cloned_packet->pos = 12345;
    const AVPacket unchanged_packet = *cloned_packet;
    const auto expect_invalid_arguments = [&](
        AVRational source_time_base, AVRational output_time_base, int output_stream_index
    ) {
        bool invalid_arguments_rejected = false;
        try {
            prepareEncodedPacketForOutput(
                *cloned_packet, source_time_base, output_time_base, output_stream_index
            );
        } catch (const std::invalid_argument&) {
            invalid_arguments_rejected = true;
        }
        assert(invalid_arguments_rejected);
        assert(cloned_packet->pts == unchanged_packet.pts);
        assert(cloned_packet->dts == unchanged_packet.dts);
        assert(cloned_packet->duration == unchanged_packet.duration);
        assert(cloned_packet->time_base.num == unchanged_packet.time_base.num);
        assert(cloned_packet->time_base.den == unchanged_packet.time_base.den);
        assert(cloned_packet->stream_index == unchanged_packet.stream_index);
        assert(cloned_packet->pos == unchanged_packet.pos);
        assert(cloned_packet.get() == original_packet);
        assert(cloned_packet->data == original_data && cloned_packet->buf == original_buffer);
        assert(cloned_packet->size == 1 && cloned_packet->data[0] == 42);
        assert(cloned_packet->flags == AV_PKT_FLAG_KEY);
    };
    const AVRational invalid_time_bases[] = {{0, 90000}, {-1, 90000}, {1, 0}, {1, -90000}};
    for (const AVRational invalid_time_base : invalid_time_bases) {
        expect_invalid_arguments(invalid_time_base, {1, 1000}, 0);
        expect_invalid_arguments({1, 90000}, invalid_time_base, 0);
    }
    expect_invalid_arguments({1, 90000}, {1, 1000}, -1);

    std::cout << "PASS: preparacao do pacote preserva bytes, ownership e timestamps desconhecidos\n";
    std::cout << "PASS: argumentos invalidos sao rejeitados sem alterar o pacote\n";

    return 0;
}
