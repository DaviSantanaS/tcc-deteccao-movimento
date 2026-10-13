#include "VideoStreamParameters.hpp"

#include <stdexcept>

VideoStreamParameters::VideoStreamParameters(
    const AVCodecParameters& source_codec_parameters,
    AVRational source_packet_timestamp_time_base
) : packet_timestamp_time_base(source_packet_timestamp_time_base) {
    if (packet_timestamp_time_base.num <= 0 ||
        packet_timestamp_time_base.den <= 0) {
        throw std::invalid_argument("A base de tempo deve ser positiva.");
    }

    codec_parameters = cloneAvCodecParameters(source_codec_parameters);
}
