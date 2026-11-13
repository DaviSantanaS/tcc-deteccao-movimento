#pragma once

#include <cstddef>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
}

bool checkIfBufferStartsWithAnnexBStartCode(const unsigned char* data, std::size_t size);
bool checkIfAnnexBContainsNalUnitType(const unsigned char* data, std::size_t size, int nal_unit_type);
bool checkIfLengthPrefixedContainsNalUnitType(
    const unsigned char* data, std::size_t size, int nal_length_size, int nal_unit_type
);
bool checkIfH264PacketContainsNalUnitType(
    const AVPacket& encoded_packet, const AVCodecParameters& codec_parameters, int nal_unit_type
);
