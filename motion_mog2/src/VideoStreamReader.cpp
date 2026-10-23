#include "VideoStreamReader.hpp"
#include "FfmpegRawVideoSource.hpp"

#include <opencv2/opencv.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

VideoStreamReader::VideoStreamReader(const std::string& input_source) {
    cv::cudacodec::VideoReaderInitParams reader_parameters;
    reader_parameters.allowFrameDrop = false;
    reader_parameters.rawMode = true;
    reader_parameters.udpSource = true;

    this->ffmpeg_video_source_ = cv::makePtr<FfmpegRawVideoSource>(input_source);
    this->nvdec_video_reader_ = cv::cudacodec::createVideoReader(
        this->ffmpeg_video_source_,
        reader_parameters
    );

    const cv::cudacodec::FormatInfo stream_format = this->nvdec_video_reader_->format();
    this->stream_fps_ = stream_format.fps;
    this->decoded_frame_width_ = stream_format.width;
    this->decoded_frame_height_ = stream_format.height;

    if (this->stream_fps_ <= 0.0) {
        throw std::runtime_error(
            "FPS invalido informado pelo stream: " + std::to_string(this->stream_fps_)
        );
    }

    if (this->decoded_frame_width_ <= 0 || this->decoded_frame_height_ <= 0) {
        throw std::runtime_error(
            "Resolucao invalida informada pelo stream: " +
            std::to_string(this->decoded_frame_width_) + "x" +
            std::to_string(this->decoded_frame_height_)
        );
    }

    double decoded_frame_retrieve_index_value = -1.0;
    if (!this->nvdec_video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_DECODED_FRAME_IDX,
            decoded_frame_retrieve_index_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter PROP_DECODED_FRAME_IDX."
        );
    }

    this->decoded_frame_retrieve_index_ =
        static_cast<std::size_t>(decoded_frame_retrieve_index_value);
}

bool VideoStreamReader::read(
    DecodedFrame& decoded_frame,
    std::vector<AvPacketPtr>& encoded_packets,
    cv::cuda::Stream& cuda_stream
) {
    if (!this->nvdec_video_reader_->grab(cuda_stream)) {
        return false;
    }

    if (!this->nvdec_video_reader_->retrieve(
            decoded_frame.decoded_frame_gpu,
            this->decoded_frame_retrieve_index_) ||
        decoded_frame.decoded_frame_gpu.empty()) {
        throw std::runtime_error("Frame decodificado nao foi recuperado.");
    }

    double encoded_packet_count_value = 0.0;
    if (!this->nvdec_video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB,
            encoded_packet_count_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter a quantidade de pacotes codificados."
        );
    }

    const std::size_t encoded_packet_count =
        static_cast<std::size_t>(encoded_packet_count_value);
    encoded_packets =
        this->ffmpeg_video_source_->dequeueEncodedPackets(encoded_packet_count);

    decoded_frame.decoded_frame_index = this->next_decoded_frame_index_;
    ++this->next_decoded_frame_index_;
    return true;
}

std::shared_ptr<const VideoStreamParameters>
VideoStreamReader::videoStreamParameters() const {
    return this->ffmpeg_video_source_->videoStreamParameters();
}

double VideoStreamReader::fps() const {
    return this->stream_fps_;
}

int VideoStreamReader::width() const {
    return this->decoded_frame_width_;
}

int VideoStreamReader::height() const {
    return this->decoded_frame_height_;
}

std::uint64_t VideoStreamReader::decodedFrameCount() const {
    return this->next_decoded_frame_index_;
}
