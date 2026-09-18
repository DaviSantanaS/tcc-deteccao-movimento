#include "FfmpegRawVideoSource.hpp"

extern "C" {
#include <libavutil/error.h>
}

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace {

std::string ffmpegErrorMessage(int error_code) {
    char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

int startCodeSize(const unsigned char* data, size_t size) {
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }

    if (size >= 4 && data[0] == 0 && data[1] == 0 &&
        data[2] == 0 && data[3] == 1) {
        return 4;
    }

    return 0;
}

bool packetAlreadyStartsWithParameterSet(
    const cv::Mat& codec_extra_data,
    const AVPacket& packet
) {
    if (codec_extra_data.empty() || packet.data == nullptr || packet.size <= 0) {
        return false;
    }

    const int extra_data_start_code_size = startCodeSize(
        codec_extra_data.data,
        codec_extra_data.total()
    );
    const int packet_start_code_size = startCodeSize(
        packet.data,
        static_cast<size_t>(packet.size)
    );

    return extra_data_start_code_size > 0 &&
           packet_start_code_size > 0 &&
           codec_extra_data.total() >
               static_cast<size_t>(extra_data_start_code_size) &&
           static_cast<size_t>(packet.size) >
               static_cast<size_t>(packet_start_code_size) &&
           codec_extra_data.data[extra_data_start_code_size] ==
               packet.data[packet_start_code_size];
}

}  // namespace

FfmpegRawVideoSource::FfmpegRawVideoSource(const std::string& rtsp_url)
    : current_packet_(allocateAvPacket()) {
    avformat_network_init();

    try {
        openInput(rtsp_url);
        initializeOpenCvFormat();
        copyCodecExtraDataFromStream();
    } catch (...) {
        closeInput();
        throw;
    }
}

FfmpegRawVideoSource::~FfmpegRawVideoSource() {
    requestStop();
    closeInput();
}

void FfmpegRawVideoSource::openInput(const std::string& rtsp_url) {
    input_context_ = avformat_alloc_context();
    if (input_context_ == nullptr) {
        throw std::runtime_error("Nao foi possivel alocar AVFormatContext.");
    }

    input_context_->interrupt_callback.callback = &interruptCallback;
    input_context_->interrupt_callback.opaque = this;

    int result = avformat_open_input(
        &input_context_,
        rtsp_url.c_str(),
        nullptr,
        nullptr
    );
    if (result < 0) {
        throw std::runtime_error(
            "Nao foi possivel abrir o stream com FFmpeg: " +
            ffmpegErrorMessage(result)
        );
    }

    result = avformat_find_stream_info(input_context_, nullptr);
    if (result < 0) {
        throw std::runtime_error(
            "Nao foi possivel obter informacoes do stream: " +
            ffmpegErrorMessage(result)
        );
    }

    result = av_find_best_stream(
        input_context_,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0
    );
    if (result < 0) {
        throw std::runtime_error(
            "Nenhum stream de video foi encontrado: " +
            ffmpegErrorMessage(result)
        );
    }

    video_stream_index_ = result;
    video_stream_ = input_context_->streams[video_stream_index_];
}

bool FfmpegRawVideoSource::getNextPacket(
    unsigned char** data,
    size_t* size
) {
    if (data == nullptr || size == nullptr) {
        setLastError("RawVideoSource recebeu ponteiro de saida nulo.");
        return false;
    }

    *data = nullptr;
    *size = 0;

    // A chamada anterior ja foi consumida pelo parser do OpenCV. O clone
    // colocado em pending_packets_ mantem os mesmos bytes vivos para o buffer.
    av_packet_unref(current_packet_.get());

    while (!stop_requested_.load()) {
        const int result = av_read_frame(
            input_context_,
            current_packet_.get()
        );

        if (result == AVERROR(EAGAIN)) {
            continue;
        }

        if (result < 0) {
            if (!stop_requested_.load()) {
                setLastError(
                    "Falha ao ler AVPacket do stream: " +
                    ffmpegErrorMessage(result)
                );
            }
            return false;
        }

        if (current_packet_->stream_index != video_stream_index_ ||
            current_packet_->data == nullptr ||
            current_packet_->size <= 0) {
            av_packet_unref(current_packet_.get());
            continue;
        }

        // av_read_frame() informa os timestamps na base temporal do AVStream.
        // Guardamos essa base no pacote para que o remux nao precise adivinha-la.
        current_packet_->time_base = video_stream_->time_base;

        try {
            AvPacketPtr stored_packet = cloneAvPacket(*current_packet_);
            std::lock_guard<std::mutex> lock(packet_mutex_);
            pending_packets_.push_back(std::move(stored_packet));
        } catch (const std::exception& error) {
            setLastError(error.what());
            return false;
        }

        const bool is_key_frame =
            (current_packet_->flags & AV_PKT_FLAG_KEY) != 0;
        last_packet_contains_key_frame_.store(is_key_frame);

        const bool prepend_codec_extra_data =
            first_video_packet_ &&
            !codec_extra_data_.empty() &&
            !packetAlreadyStartsWithParameterSet(
                codec_extra_data_,
                *current_packet_
            );

        try {
            if (prepend_codec_extra_data) {
                // O parser CUDA precisa receber SPS/PPS (H.264) ou VPS/SPS/PPS
                // (HEVC) antes do primeiro pacote quando vieram apenas no SDP.
                const size_t packet_byte_size =
                    static_cast<size_t>(current_packet_->size);
                first_parser_packet_bytes_.resize(
                    codec_extra_data_.total() + packet_byte_size
                );
                std::memcpy(
                    first_parser_packet_bytes_.data(),
                    codec_extra_data_.data,
                    codec_extra_data_.total()
                );
                std::memcpy(
                    first_parser_packet_bytes_.data() +
                        codec_extra_data_.total(),
                    current_packet_->data,
                    packet_byte_size
                );

                *data = first_parser_packet_bytes_.data();
                *size = first_parser_packet_bytes_.size();
            } else {
                // current_packet_ nao pode ser liberado antes da proxima
                // chamada, pois o parser usa este ponteiro imediatamente.
                *data = current_packet_->data;
                *size = static_cast<size_t>(current_packet_->size);
            }
        } catch (const std::exception& error) {
            setLastError(
                "Falha ao preparar pacote para o parser CUDA: " +
                std::string(error.what())
            );
            return false;
        }

        first_video_packet_ = false;
        return true;
    }

    return false;
}

bool FfmpegRawVideoSource::lastPacketContainsKeyFrame() const {
    return last_packet_contains_key_frame_.load();
}

std::vector<AvPacketPtr> FfmpegRawVideoSource::takePendingPackets(
    size_t packet_count
) {
    std::lock_guard<std::mutex> lock(packet_mutex_);

    if (pending_packets_.size() < packet_count) {
        throw std::runtime_error(
            "Fila de AVPacket dessincronizada: esperados " +
            std::to_string(packet_count) + ", disponiveis " +
            std::to_string(pending_packets_.size()) + "."
        );
    }

    std::vector<AvPacketPtr> packets;
    packets.reserve(packet_count);

    for (size_t index = 0; index < packet_count; ++index) {
        packets.push_back(std::move(pending_packets_.front()));
        pending_packets_.pop_front();
    }

    return packets;
}

std::string FfmpegRawVideoSource::lastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void FfmpegRawVideoSource::requestStop() noexcept {
    stop_requested_.store(true);
}

int FfmpegRawVideoSource::interruptCallback(void* callback_context) {
    if (callback_context == nullptr) {
        return 0;
    }

    const auto* source =
        static_cast<FfmpegRawVideoSource*>(callback_context);
    return source->stop_requested_.load() ? 1 : 0;
}

void FfmpegRawVideoSource::setLastError(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = message;
}

void FfmpegRawVideoSource::closeInput() noexcept {
    if (input_context_ != nullptr) {
        avformat_close_input(&input_context_);
    }

    video_stream_ = nullptr;
    video_stream_index_ = -1;
}
