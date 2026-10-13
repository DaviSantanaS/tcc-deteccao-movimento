#pragma once

#include "AvPacket.hpp"

extern "C" {
#include <libavcodec/bsf.h>
}

#include <memory>
#include <vector>

struct AvBitstreamFilterDeleter {
    void operator()(AVBSFContext* filter_context) const noexcept;
};

using AvBitstreamFilterPtr =
    std::unique_ptr<AVBSFContext, AvBitstreamFilterDeleter>;

[[nodiscard]] AvBitstreamFilterPtr createH264AnnexBFilter(
    const AVCodecParameters& source_codec_parameters,
    AVRational packet_timestamp_time_base
);
