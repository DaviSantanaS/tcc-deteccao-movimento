#include <FfmpegRawVideoSource.hpp>

#include <opencv2/core/utils/logger.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
}

#include <cmath>
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

FfmpegRawVideoSource::FfmpegRawVideoSource(const std::string& input_url)
    : current_encoded_frame_packet(allocateAvPacket()) {
    openInput(input_url);
    initializeOpenCvFormat();
    video_stream_parameters = std::make_shared<const VideoStreamParameters>(
        *encoded_video_stream->codecpar,
        encoded_video_stream->time_base
    );
    decoder_bitstream_filter = createH264AnnexBFilter(
        *video_stream_parameters->codec_parameters,
        video_stream_parameters->packet_timestamp_time_base
    );
    copyCodecExtraDataFromStream();
}

std::shared_ptr<const VideoStreamParameters>
FfmpegRawVideoSource::videoStreamParameters() const {
    return video_stream_parameters;
}

void FfmpegRawVideoSource::openInput(const std::string& input_url) {
    AVFormatContext* opened_input_format_context = nullptr;
    const int open_input_result = avformat_open_input(
        &opened_input_format_context,
        input_url.c_str(),
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

    encoded_video_stream =
        input_format_context->streams[selected_video_stream_index];
    encoded_video_stream_index = selected_video_stream_index;
    video_source_context = std::move(input_format_context);
}

void FfmpegRawVideoSource::initializeOpenCvFormat() {
    if (encoded_video_stream == nullptr ||
        encoded_video_stream->codecpar == nullptr) {
        throw std::runtime_error(
            "O stream de video nao possui parametros de codec."
        );
    }

    const AVCodecID codec_id = encoded_video_stream->codecpar->codec_id;
    if (codec_id != AV_CODEC_ID_H264) {
        throw std::runtime_error(
            "Codec de video nao suportado: " +
            std::string(avcodec_get_name(codec_id))
        );
    }

    video_format.codec = cv::cudacodec::Codec::H264;
    const AVCodecParameters* codec_parameters =
    encoded_video_stream->codecpar;

    if (codec_parameters->width <= 0 || codec_parameters->height <= 0) {
        throw std::runtime_error("Resolucao invalida informada pelo stream.");
    }

    const AVRational frame_rate = av_guess_frame_rate(
        video_source_context.get(),
        encoded_video_stream,
        nullptr
    );

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        throw std::runtime_error("Nao foi possivel determinar o FPS do stream.");
    }

    video_format.width = codec_parameters->width;
    video_format.height = codec_parameters->height;
    video_format.displayArea = cv::Rect(
        0, 0, video_format.width, video_format.height
    );
    video_format.fps = av_q2d(frame_rate);
}

cv::cudacodec::FormatInfo FfmpegRawVideoSource::format() const {
    return video_format;
}

void FfmpegRawVideoSource::updateFormat(
    const cv::cudacodec::FormatInfo& updated_video_format
) {
    const double previous_stream_fps = video_format.fps;

    video_format = updated_video_format;

    if (!std::isfinite(video_format.fps) || video_format.fps <= 0.0) {
        video_format.fps = previous_stream_fps;
    }

    video_format.valid = true;
}

bool FfmpegRawVideoSource::lastPacketContainsKeyFrame() const {
    return (current_encoded_frame_packet->flags & AV_PKT_FLAG_KEY) != 0;
}

bool FfmpegRawVideoSource::get(int, double& property_value) const {
    property_value = 0.0;
    return false;
}

int FfmpegRawVideoSource::getFirstFrameIdx() const {
    // A entrada RTSP do MVP nao faz seek.
    return 0;
}

void FfmpegRawVideoSource::copyCodecExtraDataFromStream() {
    const AVCodecParameters* codec_parameters =
        encoded_video_stream->codecpar;

    if (codec_parameters->extradata == nullptr ||
        codec_parameters->extradata_size <= 0) {
        video_codec_extra_data.release();
        return;
    }

    video_codec_extra_data.create(
        1,
        codec_parameters->extradata_size,
        CV_8UC1
    );
    std::memcpy(
        video_codec_extra_data.data,
        codec_parameters->extradata,
        static_cast<size_t>(codec_parameters->extradata_size)
    );
}

void FfmpegRawVideoSource::getExtraData(cv::Mat& extra_data) const {
    video_codec_extra_data.copyTo(extra_data);
}

bool FfmpegRawVideoSource::getNextPacket(
    unsigned char** decoder_packet_data,
    std::size_t* decoder_packet_size
) {
    if (decoder_packet_data == nullptr || decoder_packet_size == nullptr) {
        return false;
    }

    *decoder_packet_data = nullptr;
    *decoder_packet_size = 0;
    current_decoder_packet.reset();
    av_packet_unref(current_encoded_frame_packet.get());

    while (true) {
        const int packet_read_status = av_read_frame(
            video_source_context.get(),
            current_encoded_frame_packet.get()
        );

        if (packet_read_status == AVERROR(EAGAIN)) {
            continue;
        }

        if (packet_read_status < 0) {
            return false;
        }

        if (current_encoded_frame_packet->stream_index != encoded_video_stream_index ||
            current_encoded_frame_packet->data == nullptr ||
            current_encoded_frame_packet->size <= 0) {
            av_packet_unref(current_encoded_frame_packet.get());
            continue;
        }

        current_encoded_frame_packet->time_base = encoded_video_stream->time_base;

        try {
            auto decoder_packets = filterH264PacketToAnnexB(
                *decoder_bitstream_filter,
                *current_encoded_frame_packet
            );
            if (decoder_packets.size() != 1 || !decoder_packets.front() ||
                decoder_packets.front()->data == nullptr || decoder_packets.front()->size <= 0) {
                CV_LOG_ERROR(nullptr,
                    "O filtro H.264 deve produzir um pacote valido por entrada para manter a contagem raw do OpenCV.");
                return false;
            }

            AvPacketPtr pending_packet = cloneAvPacket(*current_encoded_frame_packet);
            {
                std::lock_guard<std::mutex> lock(encoded_frames_packets_buffer_mutex);
                encoded_frames_packets_buffer.push_back(std::move(pending_packet));
            }

            current_decoder_packet = std::move(decoder_packets.front());
            *decoder_packet_data = current_decoder_packet->data;
            *decoder_packet_size = static_cast<std::size_t>(current_decoder_packet->size);
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
    std::lock_guard<std::mutex> lock(encoded_frames_packets_buffer_mutex);

    if (encoded_frames_packets_buffer.size() < packet_count) {
        throw std::runtime_error(
            "Fila de AVPacket dessincronizada: esperados " +
            std::to_string(packet_count) + ", disponiveis " +
            std::to_string(encoded_frames_packets_buffer.size()) + "."
        );
    }

    std::vector<AvPacketPtr> packets;
    packets.reserve(packet_count);

    for (std::size_t index = 0; index < packet_count; ++index) {
        packets.push_back(std::move(encoded_frames_packets_buffer.front()));
        encoded_frames_packets_buffer.pop_front();
    }

    return packets;
}

EncodedVideoPackets FfmpegRawVideoSource::takePacketsWithStreamParameters(
    std::size_t packet_count
) {
    return {
        dequeueEncodedPackets(packet_count),
        video_stream_parameters
    };
}
