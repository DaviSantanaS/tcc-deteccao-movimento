#include "ffmpeg/AvBitstreamFilter.hpp"

extern "C" {
#include <libavutil/error.h>
}

#include <new>
#include <stdexcept>
#include <string>
#include <utility>

void AvBitstreamFilterDeleter::operator()(AVBSFContext* filter_context) const noexcept {
    av_bsf_free(&filter_context);
}

AvBitstreamFilterPtr createH264AnnexBFilter(
    const AVCodecParameters& source_codec_parameters,
    AVRational packet_timestamp_time_base
) {
    if (source_codec_parameters.codec_id != AV_CODEC_ID_H264) {
        throw std::invalid_argument("O filtro Annex B requer parametros de codec H.264.");
    }
    if (packet_timestamp_time_base.num <= 0 || packet_timestamp_time_base.den <= 0) {
        throw std::invalid_argument("A base de tempo dos pacotes deve ser positiva.");
    }

    const AVBitStreamFilter* h264_annex_b_filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (h264_annex_b_filter == nullptr) {
        throw std::runtime_error("O filtro h264_mp4toannexb nao esta disponivel.");
    }

    AVBSFContext* allocated_filter_context = nullptr;
    const int filter_allocation_status = av_bsf_alloc(h264_annex_b_filter, &allocated_filter_context);
    AvBitstreamFilterPtr filter_context{allocated_filter_context};
    if (filter_allocation_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel alocar o filtro H.264 Annex B: erro " +
            std::to_string(filter_allocation_status)
        );
    }
    if (!filter_context) {
        throw std::bad_alloc{};
    }

    const int codec_parameters_copy_status = avcodec_parameters_copy(
        filter_context->par_in,
        &source_codec_parameters
    );
    if (codec_parameters_copy_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel copiar os parametros para o filtro H.264: erro " +
            std::to_string(codec_parameters_copy_status)
        );
    }

    filter_context->time_base_in = packet_timestamp_time_base;
    const int filter_initialization_status = av_bsf_init(filter_context.get());
    if (filter_initialization_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel inicializar o filtro H.264 Annex B: erro " +
            std::to_string(filter_initialization_status)
        );
    }

    return filter_context;
}

AvPacketPtr filterH264PacketToAnnexB(
    AVBSFContext& annex_b_filter_context,
    const AVPacket& original_encoded_packet
) {
    if (original_encoded_packet.data == nullptr || original_encoded_packet.size <= 0) {
        throw std::invalid_argument("A filtragem requer um pacote codificado com dados.");
    }

    AvPacketPtr encoded_packet_for_filter = cloneAvPacket(original_encoded_packet);
    const int packet_submission_status = av_bsf_send_packet(
        &annex_b_filter_context,
        encoded_packet_for_filter.get()
    );
    if (packet_submission_status == AVERROR(EAGAIN)) {
        throw std::runtime_error("O filtro H.264 precisa ser drenado antes de receber outro pacote.");
    }
    if (packet_submission_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel enviar o pacote ao filtro H.264: erro " +
            std::to_string(packet_submission_status)
        );
    }

    AvPacketPtr annex_b_packet = allocateAvPacket();
    const int packet_reception_status = av_bsf_receive_packet(
        &annex_b_filter_context,
        annex_b_packet.get()
    );
    if (packet_reception_status == AVERROR(EAGAIN) || packet_reception_status == AVERROR_EOF) {
        throw std::runtime_error("O filtro H.264 nao produziu pacote para a entrada enviada.");
    }
    if (packet_reception_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel receber o pacote do filtro H.264: erro " +
            std::to_string(packet_reception_status)
        );
    }
    annex_b_packet->time_base = annex_b_filter_context.time_base_out;

    // O h264_mp4toannexb produz uma saida por entrada. A segunda recepcao
    // confirma isso e deixa o filtro drenado para o proximo envio.
    AvPacketPtr unexpected_extra_packet = allocateAvPacket();
    const int extra_packet_reception_status = av_bsf_receive_packet(
        &annex_b_filter_context,
        unexpected_extra_packet.get()
    );
    if (extra_packet_reception_status != AVERROR(EAGAIN) && extra_packet_reception_status != AVERROR_EOF) {
        throw std::runtime_error(
            extra_packet_reception_status < 0
                ? "Nao foi possivel drenar o filtro H.264: erro " + std::to_string(extra_packet_reception_status)
                : std::string("O filtro H.264 produziu mais de um pacote para uma unica entrada.")
        );
    }

    return annex_b_packet;
}
