#include "VideoStreamReader.hpp"

#include <opencv2/opencv.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

VideoStreamReader::VideoStreamReader(const std::string& rtsp_url) {
    cv::cudacodec::VideoReaderInitParams reader_params;
    reader_params.allowFrameDrop = false;
    reader_params.rawMode = true;
    // RTSP e uma fonte ao vivo. Isso desativa o limite de pacotes que o
    // parser aceita receber enquanto ainda procura o proximo frame.
    reader_params.udpSource = true;

    raw_video_source_ = cv::makePtr<FfmpegRawVideoSource>(rtsp_url);
    video_reader_ = cv::cudacodec::createVideoReader(
        raw_video_source_,
        reader_params
    );

    const cv::cudacodec::FormatInfo stream_format = video_reader_->format();
    stream_fps_ = stream_format.fps;
    decoded_frame_width_ = stream_format.width;
    decoded_frame_height_ = stream_format.height;

    if (stream_fps_ <= 0.0) {
        throw std::runtime_error(
            "FPS invalido informado pelo stream: " + std::to_string(stream_fps_)
        );
    }

    if (decoded_frame_width_ <= 0 || decoded_frame_height_ <= 0) {
        throw std::runtime_error(
            "Resolucao invalida informada pelo stream: " +
            std::to_string(decoded_frame_width_) + "x" +
            std::to_string(decoded_frame_height_)
        );
    }

    double decoded_frame_retrieve_index_value = -1.0;
    if (!video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_DECODED_FRAME_IDX,
            decoded_frame_retrieve_index_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter PROP_DECODED_FRAME_IDX."
        );
    }

    if (!std::isfinite(decoded_frame_retrieve_index_value) ||
        decoded_frame_retrieve_index_value < 0.0 ||
        std::floor(decoded_frame_retrieve_index_value) !=
            decoded_frame_retrieve_index_value ||
        decoded_frame_retrieve_index_value >
            static_cast<double>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error(
            "Indice invalido para recuperar o frame decodificado."
        );
    }

    decoded_frame_retrieve_index_ =
        static_cast<size_t>(decoded_frame_retrieve_index_value);
}

VideoStreamReader::~VideoStreamReader() {
    if (raw_video_source_) {
        raw_video_source_->requestStop();
    }

    video_reader_.reset();
    raw_video_source_.reset();
}

bool VideoStreamReader::read(
    DecodedFrame& decoded_frame,
    EncodedFramePackets& encoded_frame_packets,
    cv::cuda::Stream& cuda_stream
) {
    encoded_frame_packets.encoded_packets.clear();

    if (!video_reader_->grab(cuda_stream)) {
        const std::string source_error = raw_video_source_->lastError();
        if (!source_error.empty()) {
            throw std::runtime_error(source_error);
        }
        return false;
    }

    if (!video_reader_->retrieve(
            decoded_frame.decoded_frame_gpu,
            decoded_frame_retrieve_index_) ||
        decoded_frame.decoded_frame_gpu.empty()) {
        throw std::runtime_error("Frame decodificado nao foi recuperado.");
    }

    double encoded_packet_count_value = 0.0;
    if (!video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB,
            encoded_packet_count_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter a quantidade de pacotes codificados."
        );
    }

    if (!std::isfinite(encoded_packet_count_value) ||
        encoded_packet_count_value < 0.0 ||
        std::floor(encoded_packet_count_value) != encoded_packet_count_value ||
        encoded_packet_count_value >
            static_cast<double>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error(
            "Quantidade invalida de AVPackets informada pelo OpenCV."
        );
    }

    const size_t encoded_packet_count =
        static_cast<size_t>(encoded_packet_count_value);

    encoded_frame_packets.encoded_packets =
        raw_video_source_->takePendingPackets(encoded_packet_count);

    decoded_frame.decoded_frame_index = next_decoded_frame_index_;
    ++next_decoded_frame_index_;
    return true;
}

double VideoStreamReader::fps() const {
    return stream_fps_;
}

int VideoStreamReader::width() const {
    return decoded_frame_width_;
}

int VideoStreamReader::height() const {
    return decoded_frame_height_;
}

uint64_t VideoStreamReader::processedFrameCount() const {
    return next_decoded_frame_index_;
}
