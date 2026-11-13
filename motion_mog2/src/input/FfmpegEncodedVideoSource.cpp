#include "input/FfmpegEncodedVideoSource.hpp"

#include <opencv2/core/utils/logger.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
}

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

int interruptInputRead(void* interrupt_context) {
    const auto* keep_running = static_cast<const std::atomic<bool>*>(interrupt_context);
    // O handler escreve em atomic<bool> lock-free. Este callback interrompe
    // av_read_frame para a thread do OpenCV terminar mesmo com a fonte travada.
    return keep_running != nullptr && !keep_running->load() ? 1 : 0;
}

std::string getFfmpegErrorMessage(int error_code) {
    char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

}

void FfmpegEncodedVideoSource::AvFormatContextDeleter::operator()(
    AVFormatContext* input_context
) const noexcept {
    avformat_close_input(&input_context);
}

FfmpegEncodedVideoSource::FfmpegEncodedVideoSource(
    const std::string& input_source,
    const std::string& rtsp_transport,
    const std::atomic<bool>* keep_running,
    std::size_t maximum_uncounted_packet_count
)
    : current_encoded_packet_(allocateAvPacket()), keep_running_(keep_running),
      maximum_uncounted_packet_count_(maximum_uncounted_packet_count) {
    this->openInputAndSelectVideoStream(input_source, rtsp_transport, keep_running);
    this->initializeVideoFormat();
    if (this->maximum_uncounted_packet_count_ == 0) {
        // Os 40 s sao duas vezes o GOP suposto de 20 s no main. Na partida da sessao,
        // a regiao a frente cresce legitimamente ate o primeiro keyframe: os pacotes
        // anteriores a ele nao geram quadro e nao sao contabilizados. O teto precisa
        // ficar acima de um GOP inteiro para nao disparar nesse periodo.
        constexpr double maximum_uncounted_seconds = 40.0;
        constexpr std::size_t decoder_surface_margin_packet_count = 32;
        this->maximum_uncounted_packet_count_ = static_cast<std::size_t>(
            std::ceil(this->video_format_.fps * maximum_uncounted_seconds)
        ) + decoder_surface_margin_packet_count;
    }
    this->video_stream_parameters_ = std::make_shared<const VideoStreamParameters>(
        *this->encoded_video_stream_->codecpar,
        this->encoded_video_stream_->time_base
    );
    this->annex_b_filter_for_nvdec_ = createH264AnnexBFilter(
        *this->video_stream_parameters_->codec_parameters,
        this->video_stream_parameters_->packet_timestamp_time_base
    );
    this->copyVideoCodecExtraData();
}

std::shared_ptr<const VideoStreamParameters>
FfmpegEncodedVideoSource::getVideoStreamParameters() const {
    return this->video_stream_parameters_;
}

EncodedPacketQueue& FfmpegEncodedVideoSource::getEncodedPacketQueue() {
    return this->encoded_packet_queue_;
}

PacketReadEnd FfmpegEncodedVideoSource::getPacketReadEnd() const {
    return {this->packet_read_end_reason_.load(), this->packet_read_end_error_code_.load()};
}

std::size_t FfmpegEncodedVideoSource::getMaximumUncountedPacketCount() const {
    return this->maximum_uncounted_packet_count_;
}

void FfmpegEncodedVideoSource::recordPacketReadEnd(PacketReadEndReason reason, int ffmpeg_error_code) {
    // getNextPacket tem um unico escritor; publica o codigo antes do motivo e preserva a primeira causa.
    if (this->packet_read_end_reason_.load() == PacketReadEndReason::not_ended) {
        this->packet_read_end_error_code_.store(ffmpeg_error_code);
        this->packet_read_end_reason_.store(reason);
    }
}

void FfmpegEncodedVideoSource::openInputAndSelectVideoStream(
    const std::string& input_source,
    const std::string& rtsp_transport,
    const std::atomic<bool>* keep_running
) {
    AVDictionary* input_options = nullptr;
    const bool use_rtsp_transport_option = input_source.rfind("rtsp://", 0) == 0 && rtsp_transport != "auto";
    if (use_rtsp_transport_option) {
        // FFmpeg 6.1 (rtsp.c, ff_rtsp_connect) tenta UDP primeiro por padrao;
        // a opcao explicita faz o gravador usar o transporte escolhido.
        const int option_configuration_status = av_dict_set(
            &input_options, "rtsp_transport", rtsp_transport.c_str(), 0
        );
        if (option_configuration_status < 0) {
            av_dict_free(&input_options);
            throw std::runtime_error(
                "Nao foi possivel configurar o transporte RTSP: " +
                getFfmpegErrorMessage(option_configuration_status)
            );
        }
    }
    AVFormatContext* opened_input_format_context = avformat_alloc_context();
    if (opened_input_format_context == nullptr) {
        av_dict_free(&input_options);
        throw std::runtime_error("Nao foi possivel alocar o contexto de entrada FFmpeg.");
    }
    opened_input_format_context->interrupt_callback.callback = interruptInputRead;
    // AVIOInterruptCB usa void*, mas o callback apenas consulta o atomic externo,
    // que deve continuar vivo ate o fim da leitura e do contexto de entrada.
    opened_input_format_context->interrupt_callback.opaque = const_cast<std::atomic<bool>*>(keep_running);
    const int input_initialization_status = avformat_open_input(
        &opened_input_format_context,
        input_source.c_str(),
        nullptr,
        use_rtsp_transport_option ? &input_options : nullptr
    );
    av_dict_free(&input_options);
    AvFormatContextPtr input_format_context{opened_input_format_context};

    if (input_initialization_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel abrir a entrada com FFmpeg: " +
            getFfmpegErrorMessage(input_initialization_status)
        );
    }

    const int stream_information_retrieval_status =
        avformat_find_stream_info(input_format_context.get(), nullptr);
    if (stream_information_retrieval_status < 0) {
        throw std::runtime_error(
            "Nao foi possivel obter informacoes dos streams: " +
            getFfmpegErrorMessage(stream_information_retrieval_status)
        );
    }

    const int selected_video_stream_index = av_find_best_stream(
        input_format_context.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0
    );

    if (selected_video_stream_index < 0) {
        throw std::runtime_error(
            "Nenhum stream de video foi encontrado: " +
            getFfmpegErrorMessage(selected_video_stream_index)
        );
    }

    this->encoded_video_stream_ =
        input_format_context->streams[selected_video_stream_index];
    this->encoded_video_stream_index_ = selected_video_stream_index;
    this->video_source_context_ = std::move(input_format_context);
}

void FfmpegEncodedVideoSource::initializeVideoFormat() {
    if (this->encoded_video_stream_ == nullptr ||
        this->encoded_video_stream_->codecpar == nullptr) {
        throw std::runtime_error(
            "O stream de video nao possui parametros de codec."
        );
    }

    const AVCodecID codec_id = this->encoded_video_stream_->codecpar->codec_id;
    if (codec_id != AV_CODEC_ID_H264) {
        throw std::runtime_error(
            "Codec de video nao suportado: " +
            std::string(avcodec_get_name(codec_id))
        );
    }

    this->video_format_.codec = cv::cudacodec::Codec::H264;
    const AVCodecParameters* codec_parameters =
        this->encoded_video_stream_->codecpar;

    if (codec_parameters->width <= 0 || codec_parameters->height <= 0) {
        throw std::runtime_error("Resolucao invalida informada pelo stream.");
    }

    const AVRational frame_rate = av_guess_frame_rate(
        this->video_source_context_.get(),
        this->encoded_video_stream_,
        nullptr
    );

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        throw std::runtime_error("Nao foi possivel determinar o FPS do stream.");
    }

    this->video_format_.width = codec_parameters->width;
    this->video_format_.height = codec_parameters->height;
    this->video_format_.displayArea = cv::Rect(
        0, 0, this->video_format_.width, this->video_format_.height
    );
    this->video_format_.fps = av_q2d(frame_rate);
}

cv::cudacodec::FormatInfo FfmpegEncodedVideoSource::format() const {
    return this->video_format_;
}

void FfmpegEncodedVideoSource::updateFormat(
    const cv::cudacodec::FormatInfo& updated_video_format
) {
    const double previous_stream_fps = this->video_format_.fps;

    this->video_format_ = updated_video_format;

    if (!std::isfinite(this->video_format_.fps) || this->video_format_.fps <= 0.0) {
        this->video_format_.fps = previous_stream_fps;
    }

    this->video_format_.valid = true;
}

bool FfmpegEncodedVideoSource::lastPacketContainsKeyFrame() const {
    return (this->current_encoded_packet_->flags & AV_PKT_FLAG_KEY) != 0;
}

bool FfmpegEncodedVideoSource::get(int, double& property_value) const {
    property_value = 0.0;
    return false;
}

int FfmpegEncodedVideoSource::getFirstFrameIdx() const {
    return 0;
}

void FfmpegEncodedVideoSource::copyVideoCodecExtraData() {
    const AVCodecParameters* codec_parameters =
        this->encoded_video_stream_->codecpar;

    if (codec_parameters->extradata == nullptr ||
        codec_parameters->extradata_size <= 0) {
        this->video_codec_extra_data_.release();
        return;
    }

    this->video_codec_extra_data_.create(
        1,
        codec_parameters->extradata_size,
        CV_8UC1
    );
    std::memcpy(
        this->video_codec_extra_data_.data,
        codec_parameters->extradata,
        static_cast<std::size_t>(codec_parameters->extradata_size)
    );
}

void FfmpegEncodedVideoSource::getExtraData(cv::Mat& extra_data) const {
    this->video_codec_extra_data_.copyTo(extra_data);
}

bool FfmpegEncodedVideoSource::getNextPacket(
    unsigned char** encoded_packet_data_for_nvdec,
    std::size_t* encoded_packet_size_bytes_for_nvdec
) {
    if (encoded_packet_data_for_nvdec == nullptr || encoded_packet_size_bytes_for_nvdec == nullptr) {
        this->recordPacketReadEnd(PacketReadEndReason::packet_preparation_error, 0);
        return false;
    }

    *encoded_packet_data_for_nvdec = nullptr;
    *encoded_packet_size_bytes_for_nvdec = 0;
    this->current_encoded_packet_for_nvdec_.reset();
    av_packet_unref(this->current_encoded_packet_.get());

    while (true) {
        const int packet_retrieval_status = av_read_frame(
            this->video_source_context_.get(),
            this->current_encoded_packet_.get()
        );

        if (packet_retrieval_status == AVERROR(EAGAIN)) {
            continue;
        }

        if (packet_retrieval_status < 0) {
            const PacketReadEndReason reason = packet_retrieval_status == AVERROR_EOF
                ? PacketReadEndReason::end_of_stream
                : packet_retrieval_status == AVERROR_EXIT
                    ? PacketReadEndReason::interrupted : PacketReadEndReason::read_error;
            this->recordPacketReadEnd(reason, packet_retrieval_status);
            return false;
        }

        // FFmpeg n6.1, libavformat/utils.c:87-100: append_packet_chunked pode devolver
        // sucesso com pacote truncado apos interrupcao; nao o envia ao filtro nem a fila.
        if (this->keep_running_ != nullptr && !this->keep_running_->load()) {
            av_packet_unref(this->current_encoded_packet_.get());
            this->recordPacketReadEnd(PacketReadEndReason::interrupted, AVERROR_EXIT);
            return false;
        }

        if (this->current_encoded_packet_->stream_index != this->encoded_video_stream_index_ ||
            this->current_encoded_packet_->data == nullptr ||
            this->current_encoded_packet_->size <= 0) {
            av_packet_unref(this->current_encoded_packet_.get());
            continue;
        }

        this->current_encoded_packet_->time_base = this->encoded_video_stream_->time_base;

        try {
            auto encoded_packets_for_nvdec = filterH264PacketToAnnexB(
                *this->annex_b_filter_for_nvdec_,
                *this->current_encoded_packet_
            );
            if (encoded_packets_for_nvdec.size() != 1 || !encoded_packets_for_nvdec.front() ||
                encoded_packets_for_nvdec.front()->data == nullptr || encoded_packets_for_nvdec.front()->size <= 0) {
                CV_LOG_ERROR(nullptr,
                    "O filtro H.264 deve produzir um pacote valido por entrada para manter a contagem raw do OpenCV.");
                this->recordPacketReadEnd(PacketReadEndReason::packet_preparation_error, 0);
                return false;
            }

            const std::size_t uncounted_packet_count = this->encoded_packet_queue_.getUncountedPacketCount();
            if (uncounted_packet_count >= this->maximum_uncounted_packet_count_) {
                CV_LOG_ERROR(nullptr, "Regiao a frente da fronteira atingiu " << uncounted_packet_count
                    << " pacotes sem contabilizacao pelo decoder; leitura encerrada.");
                this->recordPacketReadEnd(PacketReadEndReason::decoder_stalled, 0);
                return false;
            }

            AvPacketPtr encoded_packet_for_queue = cloneAvPacket(*this->current_encoded_packet_);
            this->encoded_packet_queue_.pushPacket(std::move(encoded_packet_for_queue));

            this->current_encoded_packet_for_nvdec_ = std::move(encoded_packets_for_nvdec.front());
            *encoded_packet_data_for_nvdec = this->current_encoded_packet_for_nvdec_->data;
            *encoded_packet_size_bytes_for_nvdec = static_cast<std::size_t>(this->current_encoded_packet_for_nvdec_->size);
            return true;
        } catch (const std::exception& error) {
            CV_LOG_ERROR(nullptr, "Falha ao preparar o pacote para NVDEC: " << error.what());
            this->recordPacketReadEnd(PacketReadEndReason::packet_preparation_error, 0);
            return false;
        }
    }
}

std::vector<AvPacketPtr> FfmpegEncodedVideoSource::dequeueEncodedPackets(
    std::size_t packet_count
) {
    // A regiao contabilizada esta vazia na entrada, pois todo chamador
    // retira todos os pacotes que avanca antes de chamar novamente.
    this->encoded_packet_queue_.advanceCountedPackets(packet_count);
    return this->encoded_packet_queue_.takeCountedPackets();
}

EncodedVideoPackets FfmpegEncodedVideoSource::dequeueEncodedPacketsWithStreamParameters(
    std::size_t packet_count
) {
    return {
        this->dequeueEncodedPackets(packet_count),
        this->video_stream_parameters_
    };
}
