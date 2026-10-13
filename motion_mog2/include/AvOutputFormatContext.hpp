#pragma once


extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>

struct VideoStreamParameters;

// O contexto nao fecha AVIOContext nem finaliza uma gravacao.
struct AvOutputFormatContextDeleter {
    void operator()(AVFormatContext* output_context) const noexcept;
};

using AvOutputFormatContextPtr =
    std::unique_ptr<AVFormatContext, AvOutputFormatContextDeleter>;

[[nodiscard]] AvOutputFormatContextPtr allocateMp4OutputContext();

// A stream pertence ao contexto; a referencia expira quando ele e liberado.
// Apos falha de alocacao ou copia da stream, descarte o contexto por RAII.
[[nodiscard]] AVStream& createMp4VideoStream(
    AVFormatContext& mp4_output_context,
    const VideoStreamParameters& video_stream_parameters
);
