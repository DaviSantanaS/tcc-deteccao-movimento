#include "FfmpegRawVideoSource.hpp"

#include <opencv2/core/utils/logger.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
}

#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

std::string ffmpegErrorMessage(int error_code) {
    char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

}  // namespace

void FfmpegRawVideoSource::AvFormatContextDeleter::operator()(
    AVFormatContext* input_context
) const noexcept {
    avformat_close_input(&input_context);
}

FfmpegRawVideoSource::FfmpegRawVideoSource(const std::string& input_source)
    : current_encoded_packet_(allocateAvPacket()) {
    this->openInputAndSelectVideoStream(input_source);
    this->initializeVideoFormat();
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
FfmpegRawVideoSource::videoStreamParameters() const {
    return this->video_stream_parameters_;
}

void FfmpegRawVideoSource::openInputAndSelectVideoStream(const std::string& input_source) {
    AVFormatContext* opened_input_format_context = nullptr;
    const int open_input_result = avformat_open_input(
        &opened_input_format_context,
        input_source.c_str(),
        nullptr,
        nullptr
    );
    AvFormatContextPtr input_format_context{opened_input_format_context};

    if (open_input_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel abrir a entrada com FFmpeg: " +
            ffmpegErrorMessage(open_input_result)
        );
    }

    const int find_stream_info_result =
        avformat_find_stream_info(input_format_context.get(), nullptr);
    if (find_stream_info_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel obter informacoes dos streams: " +
            ffmpegErrorMessage(find_stream_info_result)
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
            ffmpegErrorMessage(selected_video_stream_index)
        );
    }

    this->encoded_video_stream_ =
        input_format_context->streams[selected_video_stream_index];
    this->encoded_video_stream_index_ = selected_video_stream_index;
    this->video_source_context_ = std::move(input_format_context);
}

void FfmpegRawVideoSource::initializeVideoFormat() {
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

cv::cudacodec::FormatInfo FfmpegRawVideoSource::format() const {
    return this->video_format_;
}

void FfmpegRawVideoSource::updateFormat(
    const cv::cudacodec::FormatInfo& updated_video_format
) {
    const double previous_stream_fps = this->video_format_.fps;

    this->video_format_ = updated_video_format;

    if (!std::isfinite(this->video_format_.fps) || this->video_format_.fps <= 0.0) {
        this->video_format_.fps = previous_stream_fps;
    }

    this->video_format_.valid = true;
}

bool FfmpegRawVideoSource::lastPacketContainsKeyFrame() const {
    return (this->current_encoded_packet_->flags & AV_PKT_FLAG_KEY) != 0;
}

bool FfmpegRawVideoSource::get(int, double& property_value) const {
    property_value = 0.0;
    return false;
}

int FfmpegRawVideoSource::getFirstFrameIdx() const {
    // A entrada RTSP do MVP nao faz seek.
    return 0;
}

void FfmpegRawVideoSource::copyVideoCodecExtraData() {
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

void FfmpegRawVideoSource::getExtraData(cv::Mat& extra_data) const {
    this->video_codec_extra_data_.copyTo(extra_data);
}

bool FfmpegRawVideoSource::getNextPacket(
    unsigned char** encoded_packet_data_for_nvdec,
    std::size_t* encoded_packet_size_bytes_for_nvdec
) {
    if (encoded_packet_data_for_nvdec == nullptr || encoded_packet_size_bytes_for_nvdec == nullptr) {
        return false;
    }

    *encoded_packet_data_for_nvdec = nullptr;
    *encoded_packet_size_bytes_for_nvdec = 0;
    this->current_encoded_packet_for_nvdec_.reset();
    av_packet_unref(this->current_encoded_packet_.get());

    while (true) {
        const int packet_read_status = av_read_frame(
            this->video_source_context_.get(),
            this->current_encoded_packet_.get()
        );

        if (packet_read_status == AVERROR(EAGAIN)) {
            continue;
        }

        if (packet_read_status < 0) {
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
                return false;
            }

            AvPacketPtr encoded_packet_to_enqueue = cloneAvPacket(*this->current_encoded_packet_);
            {
                std::lock_guard<std::mutex> lock(this->encoded_packets_queue_mutex_);
                this->encoded_packets_queue_.push_back(std::move(encoded_packet_to_enqueue));
            }

            this->current_encoded_packet_for_nvdec_ = std::move(encoded_packets_for_nvdec.front());
            *encoded_packet_data_for_nvdec = this->current_encoded_packet_for_nvdec_->data;
            *encoded_packet_size_bytes_for_nvdec = static_cast<std::size_t>(this->current_encoded_packet_for_nvdec_->size);
            return true;
        } catch (const std::exception& error) {
            CV_LOG_ERROR(nullptr, "Falha ao preparar o pacote para NVDEC: " << error.what());
            return false;
        }
    }
}

std::vector<AvPacketPtr> FfmpegRawVideoSource::dequeueEncodedPackets(
    std::size_t packet_count
) {
    std::lock_guard<std::mutex> lock(this->encoded_packets_queue_mutex_);

    if (this->encoded_packets_queue_.size() < packet_count) {
        throw std::runtime_error(
            "Fila de AVPacket dessincronizada: esperados " +
            std::to_string(packet_count) + ", disponiveis " +
            std::to_string(this->encoded_packets_queue_.size()) + "."
        );
    }

    std::vector<AvPacketPtr> dequeued_packets;
    dequeued_packets.reserve(packet_count);

    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        dequeued_packets.push_back(std::move(this->encoded_packets_queue_.front()));
        this->encoded_packets_queue_.pop_front();
    }

    return dequeued_packets;
}

EncodedVideoPackets FfmpegRawVideoSource::dequeueEncodedPacketsWithStreamParameters(
    std::size_t packet_count
) {
    return {
        this->dequeueEncodedPackets(packet_count),
        this->video_stream_parameters_
    };
}
