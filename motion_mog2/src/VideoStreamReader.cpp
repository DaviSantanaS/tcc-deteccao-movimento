#include "VideoStreamReader.hpp"
#include "FfmpegRawVideoSource.hpp"

#include <opencv2/opencv.hpp>

#include <stdexcept>

VideoStreamReader::VideoStreamReader(const std::string& rtsp_url) {
    cv::cudacodec::VideoReaderInitParams reader_params;
    reader_params.allowFrameDrop = false;
    reader_params.rawMode = true;
    reader_params.udpSource = true;

    ffmpeg_video_source_ = cv::makePtr<FfmpegRawVideoSource>(rtsp_url);
    nvdec_video_reader_ = cv::cudacodec::createVideoReader(
        ffmpeg_video_source_,
        reader_params
    );

    const cv::cudacodec::FormatInfo stream_format = nvdec_video_reader_->format();
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
    if (!nvdec_video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_DECODED_FRAME_IDX,
            decoded_frame_retrieve_index_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter PROP_DECODED_FRAME_IDX."
        );
    }

    decoded_frame_retrieve_index_ =
        static_cast<size_t>(decoded_frame_retrieve_index_value);
}

bool VideoStreamReader::read(
    DecodedFrame& decoded_frame,
    std::vector<AvPacketPtr>& encoded_packets,
    cv::cuda::Stream& cuda_stream
) {
    if (!nvdec_video_reader_->grab(cuda_stream)) {
        return false;
    }

    if (!nvdec_video_reader_->retrieve(
            decoded_frame.decoded_frame_gpu,
            decoded_frame_retrieve_index_) ||
        decoded_frame.decoded_frame_gpu.empty()) {
        throw std::runtime_error("Frame decodificado nao foi recuperado.");
    }

    double encoded_packet_count_from_grab_value = 0.0;
    if (!nvdec_video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB,
            encoded_packet_count_from_grab_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter a quantidade de pacotes codificados."
        );
    }

    const size_t encoded_packet_count_from_grab =
        static_cast<size_t>(encoded_packet_count_from_grab_value);
    encoded_packets =
        ffmpeg_video_source_->dequeueEncodedPackets(encoded_packet_count_from_grab);

    decoded_frame.decoded_frame_index = next_decoded_frame_index_;
    ++next_decoded_frame_index_;
    return true;
}

std::shared_ptr<const VideoStreamParameters>
VideoStreamReader::videoStreamParameters() const {
    return ffmpeg_video_source_->videoStreamParameters();
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
