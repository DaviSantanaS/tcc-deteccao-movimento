#include "AvBitstreamFilter.hpp"

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
    const int filter_allocation_result = av_bsf_alloc(h264_annex_b_filter, &allocated_filter_context);
    AvBitstreamFilterPtr filter_context{allocated_filter_context};
    if (filter_allocation_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel alocar o filtro H.264 Annex B: erro " +
            std::to_string(filter_allocation_result)
        );
    }
    if (!filter_context) {
        throw std::bad_alloc{};
    }

    const int codec_parameters_copy_result = avcodec_parameters_copy(
        filter_context->par_in,
        &source_codec_parameters
    );
    if (codec_parameters_copy_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel copiar os parametros para o filtro H.264: erro " +
            std::to_string(codec_parameters_copy_result)
        );
    }

    filter_context->time_base_in = packet_timestamp_time_base;
    const int filter_initialization_result = av_bsf_init(filter_context.get());
    if (filter_initialization_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel inicializar o filtro H.264 Annex B: erro " +
            std::to_string(filter_initialization_result)
        );
    }

    return filter_context;
}

std::vector<AvPacketPtr> filterH264PacketToAnnexB(
    AVBSFContext& annex_b_filter_context,
    const AVPacket& original_encoded_packet
) {
    if (original_encoded_packet.data == nullptr || original_encoded_packet.size <= 0) {
        throw std::invalid_argument("A filtragem requer um pacote codificado com dados.");
    }

    AvPacketPtr encoded_packet_for_filter = cloneAvPacket(original_encoded_packet);
    const int packet_send_result = av_bsf_send_packet(
        &annex_b_filter_context,
        encoded_packet_for_filter.get()
    );
    if (packet_send_result == AVERROR(EAGAIN)) {
        throw std::runtime_error("O filtro H.264 precisa ser drenado antes de receber outro pacote.");
    }
    if (packet_send_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel enviar o pacote ao filtro H.264: erro " +
            std::to_string(packet_send_result)
        );
    }

    std::vector<AvPacketPtr> annex_b_packets;
    while (true) {
        AvPacketPtr annex_b_packet = allocateAvPacket();
        const int packet_receive_result = av_bsf_receive_packet(
            &annex_b_filter_context,
            annex_b_packet.get()
        );
        if (packet_receive_result == AVERROR(EAGAIN) || packet_receive_result == AVERROR_EOF) {
            return annex_b_packets;
        }
        if (packet_receive_result < 0) {
            throw std::runtime_error(
                "Nao foi possivel receber o pacote do filtro H.264: erro " +
                std::to_string(packet_receive_result)
            );
        }

        annex_b_packet->time_base = annex_b_filter_context.time_base_out;
        annex_b_packets.push_back(std::move(annex_b_packet));
    }
}
