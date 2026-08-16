#include "VideoStreamReader.hpp"

#include <opencv2/opencv.hpp>

#include <stdexcept>
#include <utility>

VideoStreamReader::VideoStreamReader(const std::string& rtsp_url) {
    cv::cudacodec::VideoReaderInitParams reader_params;
    reader_params.allowFrameDrop = true;
    reader_params.rawMode = true;
    reader_params.udpSource = true;

    std::vector<int> source_params;
    video_reader_ = cv::cudacodec::createVideoReader(
        rtsp_url,
        source_params,
        reader_params
    );

    const cv::cudacodec::FormatInfo stream_format = video_reader_->format();
    stream_fps_ = stream_format.fps;
    frame_width_ = stream_format.width;
    frame_height_ = stream_format.height;

    if (stream_fps_ <= 0.0) {
        throw std::runtime_error(
            "FPS invalido informado pelo stream: " + std::to_string(stream_fps_)
        );
    }

    if (frame_width_ <= 0 || frame_height_ <= 0) {
        throw std::runtime_error(
            "Resolucao invalida informada pelo stream: " +
            std::to_string(frame_width_) + "x" + std::to_string(frame_height_)
        );
    }

    double decoded_frame_index_value = -1.0;
    if (!video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_DECODED_FRAME_IDX,
            decoded_frame_index_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter PROP_DECODED_FRAME_IDX."
        );
    }

    double raw_package_base_index_value = -1.0;
    if (!video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_RAW_PACKAGES_BASE_INDEX,
            raw_package_base_index_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter PROP_RAW_PACKAGES_BASE_INDEX."
        );
    }

    decoded_frame_index_ = static_cast<size_t>(decoded_frame_index_value);
    raw_package_base_index_ = static_cast<size_t>(raw_package_base_index_value);
}

bool VideoStreamReader::read(
    VideoFrameData& frame_data,
    cv::cuda::Stream& cuda_stream
) {
    if (!video_reader_->grab(cuda_stream)) {
        return false;
    }

    if (!video_reader_->retrieve(
            frame_data.decoded_frame_gpu,
            decoded_frame_index_) ||
        frame_data.decoded_frame_gpu.empty()) {
        throw std::runtime_error("Frame decodificado nao foi recuperado.");
    }

    double raw_package_count_value = 0.0;
    if (!video_reader_->get(
            cv::cudacodec::VideoReaderProps::PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB,
            raw_package_count_value)) {
        throw std::runtime_error(
            "Nao foi possivel obter a quantidade de pacotes codificados."
        );
    }

    const int raw_package_count = static_cast<int>(raw_package_count_value);
    const auto packet_time = std::chrono::steady_clock::now();

    frame_data.encoded_packets.clear();
    frame_data.encoded_packets.reserve(
        raw_package_count > 0 ? static_cast<size_t>(raw_package_count) : 0
    );

    for (int package_offset = 0;
         package_offset < raw_package_count;
         ++package_offset) {
        const size_t package_index =
            raw_package_base_index_ + static_cast<size_t>(package_offset);

        cv::Mat raw_packet;
        if (!video_reader_->retrieve(raw_packet, package_index) || raw_packet.empty()) {
            continue;
        }

        double key_frame_value = static_cast<double>(package_index);
        bool has_key_frame = false;
        if (video_reader_->get(
                cv::cudacodec::VideoReaderProps::PROP_LRF_HAS_KEY_FRAME,
                key_frame_value)) {
            has_key_frame = key_frame_value != 0.0;
        }

        const size_t packet_size_bytes =
            raw_packet.total() * raw_packet.elemSize();

        EncodedPacket packet;
        packet.data.assign(
            raw_packet.data,
            raw_packet.data + packet_size_bytes
        );
        packet.received_at = packet_time;
        packet.has_key_frame = has_key_frame;

        frame_data.encoded_packets.push_back(std::move(packet));
    }

    frame_data.frame_index = next_frame_index_;
    ++next_frame_index_;
    return true;
}

double VideoStreamReader::fps() const {
    return stream_fps_;
}

int VideoStreamReader::width() const {
    return frame_width_;
}

int VideoStreamReader::height() const {
    return frame_height_;
}

uint64_t VideoStreamReader::processedFrameCount() const {
    return next_frame_index_;
}
