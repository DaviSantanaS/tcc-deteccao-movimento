#pragma once

#include "AvCodecParameters.hpp"

extern "C" {
#include <libavutil/rational.h>
}

#include <memory>

struct VideoStreamParameters {
    VideoStreamParameters(
        const AVCodecParameters& source_codec_parameters,
        AVRational source_packet_timestamp_time_base
    );

    std::shared_ptr<const AVCodecParameters> codec_parameters;
    AVRational packet_timestamp_time_base;
};
