#include "input/VideoStreamReader.hpp"
#include "input/FfmpegEncodedVideoSource.hpp"

#include <opencv2/opencv.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

VideoStreamReader::VideoStreamReader(
    const std::string& input_source,
    std::shared_ptr<EncodedVideoBuffer> encoded_video_buffer
) {
    cv::cudacodec::VideoReaderInitParams reader_parameters;
    reader_parameters.allowFrameDrop = false;
    // Sem modo raw: os pacotes originais ja sao preservados pela fonte no
    // EncodedVideoBuffer, entao a fila raw interna do OpenCV seria uma copia extra.
    reader_parameters.rawMode = false;
    reader_parameters.udpSource = true;

    this->ffmpeg_video_source_ = cv::makePtr<FfmpegEncodedVideoSource>(
        input_source, std::move(encoded_video_buffer)
    );
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

    double decoded_frame_retrieval_index_value = -1.0;
    if (!this->nvdec_video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_DECODED_FRAME_IDX,
            decoded_frame_retrieval_index_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter PROP_DECODED_FRAME_IDX."
        );
    }

    this->decoded_frame_retrieval_index_ =
        static_cast<std::size_t>(decoded_frame_retrieval_index_value);
}

bool VideoStreamReader::read(DecodedFrame& decoded_frame, cv::cuda::Stream& cuda_stream) {
    if (!this->nvdec_video_reader_->grab(cuda_stream)) {
        return false;
    }

    if (!this->nvdec_video_reader_->retrieve(
            decoded_frame.decoded_frame_gpu,
            this->decoded_frame_retrieval_index_) ||
        decoded_frame.decoded_frame_gpu.empty()) {
        throw std::runtime_error("Frame decodificado nao foi recuperado.");
    }

    decoded_frame.decoded_frame_index = this->next_decoded_frame_index_;
    ++this->next_decoded_frame_index_;
    return true;
}

std::shared_ptr<const VideoStreamParameters>
VideoStreamReader::getVideoStreamParameters() const {
    return this->ffmpeg_video_source_->getVideoStreamParameters();
}

double VideoStreamReader::getFramesPerSecond() const {
    return this->stream_fps_;
}

int VideoStreamReader::getWidth() const {
    return this->decoded_frame_width_;
}

int VideoStreamReader::getHeight() const {
    return this->decoded_frame_height_;
}

std::uint64_t VideoStreamReader::getDecodedFrameCount() const {
    return this->next_decoded_frame_index_;
}
