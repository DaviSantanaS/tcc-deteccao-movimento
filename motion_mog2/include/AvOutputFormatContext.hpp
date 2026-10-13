#pragma once

#include "AvOutputIoContext.hpp"

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

// pb referencia o I/O: mantenha-o vivo ate liberar o contexto MP4.
// Em falha FFmpeg, descarte o contexto antes de fechar o I/O.
void writeMp4Header(
    AVFormatContext& mp4_output_context,
    const AvOutputIoContextPtr& output_io_context
);

// Exige cabecalho escrito com sucesso, saida nao finalizada e I/O ainda vivo.
// O FFmpeg consome os dados e esvazia o pacote mesmo em erro; o dono do AVPacket nao muda.
// Apos erro FFmpeg, descarte o contexto antes de fechar o I/O.
void writeMp4Packet(
    AVFormatContext& mp4_output_context,
    AVPacket& encoded_packet
);
