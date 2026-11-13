#include "ffmpeg/H264NalUnits.hpp"

#include <cstddef>

bool checkIfBufferStartsWithAnnexBStartCode(const unsigned char* data, std::size_t size) {
    if (data == nullptr || size < 3 || data[0] != 0 || data[1] != 0) {
        return false;
    }
    return data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1);
}

bool checkIfAnnexBContainsNalUnitType(const unsigned char* data, std::size_t size, int nal_unit_type) {
    if (data == nullptr || size < 4 || nal_unit_type < 0 || nal_unit_type > 31) {
        return false;
    }
    for (std::size_t offset = 0; offset <= size - 4; ++offset) {
        if (!checkIfBufferStartsWithAnnexBStartCode(data + offset, size - offset)) {
            continue;
        }
        const std::size_t start_code_size = data[offset + 2] == 1 ? 3 : 4;
        if (start_code_size < size - offset && (data[offset + start_code_size] & 0x1F) == nal_unit_type) {
            return true;
        }
    }
    return false;
}

bool checkIfLengthPrefixedContainsNalUnitType(
    const unsigned char* data, std::size_t size, int nal_length_size, int nal_unit_type
) {
    if (data == nullptr || nal_length_size < 1 || nal_length_size > 4 || nal_unit_type < 0 || nal_unit_type > 31) {
        return false;
    }
    const std::size_t prefix_size = static_cast<std::size_t>(nal_length_size);
    std::size_t offset = 0;
    while (offset < size) {
        if (prefix_size > size - offset) {
            return false;
        }
        std::size_t nal_size = 0;
        for (std::size_t byte_index = 0; byte_index < prefix_size; ++byte_index) {
            nal_size = (nal_size << 8) | data[offset + byte_index];
        }
        offset += prefix_size;
        if (nal_size == 0 || nal_size > size - offset) {
            return false;
        }
        if ((data[offset] & 0x1F) == nal_unit_type) {
            return true;
        }
        offset += nal_size;
    }
    return false;
}

bool checkIfH264PacketContainsNalUnitType(
    const AVPacket& encoded_packet, const AVCodecParameters& codec_parameters, int nal_unit_type
) {
    if (encoded_packet.data == nullptr || encoded_packet.size <= 0) {
        return false;
    }
    const std::size_t packet_size = static_cast<std::size_t>(encoded_packet.size);
    if (codec_parameters.extradata == nullptr || codec_parameters.extradata_size <= 0 ||
        checkIfBufferStartsWithAnnexBStartCode(
            codec_parameters.extradata, static_cast<std::size_t>(codec_parameters.extradata_size))) {
        return checkIfAnnexBContainsNalUnitType(encoded_packet.data, packet_size, nal_unit_type);
    }
    if (codec_parameters.extradata_size >= 7 && codec_parameters.extradata[0] == 1) {
        // avcC lengthSizeMinusOne: FFmpeg n6.1, h264_mp4toannexb_bsf.c:71-73.
        const int nal_length_size = (codec_parameters.extradata[4] & 0x03) + 1;
        return checkIfLengthPrefixedContainsNalUnitType(
            encoded_packet.data, packet_size, nal_length_size, nal_unit_type
        );
    }
    return false;
}
