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

// O contexto retornado por createH264AnnexBFilter deve estar sem saidas pendentes.
[[nodiscard]] std::vector<AvPacketPtr> filterH264PacketToAnnexB(
    AVBSFContext& initialized_filter_context,
    const AVPacket& encoded_packet
);
