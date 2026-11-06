#pragma once

#include "ffmpeg/AvPacket.hpp"

extern "C" {
#include <libavcodec/bsf.h>
}

#include <memory>

struct AvBitstreamFilterDeleter {
    void operator()(AVBSFContext* filter_context) const noexcept;
};

using AvBitstreamFilterPtr =
    std::unique_ptr<AVBSFContext, AvBitstreamFilterDeleter>;

[[nodiscard]] AvBitstreamFilterPtr createH264AnnexBFilter(
    const AVCodecParameters& source_codec_parameters,
    AVRational packet_timestamp_time_base
);

// Envia um pacote ao filtro e devolve exatamente um pacote filtrado.
// Lanca excecao se o filtro nao produzir saida ou produzir mais de um pacote.
[[nodiscard]] AvPacketPtr filterH264PacketToAnnexB(
    AVBSFContext& annex_b_filter_context,
    const AVPacket& original_encoded_packet
);
