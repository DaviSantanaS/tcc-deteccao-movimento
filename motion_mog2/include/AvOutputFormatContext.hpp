#pragma once

#include "AvOutputIoContext.hpp"
#include "AvPacket.hpp"
#include "EncodedVideoPackets.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>
#include <vector>

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

// Exige cabecalho inicializado com os parametros da fonte e I/O ainda vivo.
// Lotes nao vazios devem compartilhar a mesma instancia dos parametros da fonte.
// Os lotes transferidos sao liberados mesmo em falha de validacao.
// Falha de escrita pode deixar saida parcial: descarte o contexto sem tentar novamente.
void writeMp4Packets(
    AVFormatContext& mp4_output_context,
    std::vector<EncodedVideoPackets> encoded_packet_batches
);

// Adaptacao temporaria para a interface de lotes acima, com as mesmas precondicoes.
// Os argumentos transferidos sao liberados tambem em erro.
void writeMp4Packets(
    AVFormatContext& mp4_output_context,
    std::vector<AvPacketPtr> encoded_packets,
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters
);

// Exige cabecalho escrito com sucesso e nenhuma falha anterior de gravacao.
// Apos chamar o FFmpeg, desassocia pb mesmo em erro; o I/O continua com seu proprietario.
// Nao finalize novamente: descarte o contexto antes de fechar o I/O.
void writeMp4Trailer(AVFormatContext& mp4_output_context);
