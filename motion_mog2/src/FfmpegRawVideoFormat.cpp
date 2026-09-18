#include "FfmpegRawVideoSource.hpp"

#include <opencv2/videoio.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

cv::cudacodec::Codec toOpenCvCodec(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return cv::cudacodec::Codec::H264;
        case AV_CODEC_ID_HEVC:
            return cv::cudacodec::Codec::HEVC;
        default:
            throw std::runtime_error(
                "Codec nao suportado pelo leitor do TCC: " +
                std::string(avcodec_get_name(codec_id))
            );
    }
}

cv::cudacodec::ChromaFormat chromaFormatFromPixelFormat(
    AVPixelFormat pixel_format
) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
    if (descriptor == nullptr) {
        return cv::cudacodec::ChromaFormat::YUV420;
    }

    if (descriptor->nb_components == 1) {
        return cv::cudacodec::ChromaFormat::Monochrome;
    }

    if (descriptor->log2_chroma_w == 1 && descriptor->log2_chroma_h == 1) {
        return cv::cudacodec::ChromaFormat::YUV420;
    }

    if (descriptor->log2_chroma_w == 1 && descriptor->log2_chroma_h == 0) {
        return cv::cudacodec::ChromaFormat::YUV422;
    }

    return cv::cudacodec::ChromaFormat::YUV444;
}

int bitDepthMinusEightFromPixelFormat(AVPixelFormat pixel_format) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
    if (descriptor == nullptr || descriptor->nb_components == 0) {
        return 0;
    }

    return std::max(0, descriptor->comp[0].depth - 8);
}

double streamFramesPerSecond(
    AVFormatContext* input_context,
    AVStream* video_stream
) {
    AVRational frame_rate = av_guess_frame_rate(
        input_context,
        video_stream,
        nullptr
    );

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = video_stream->avg_frame_rate;
    }

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = video_stream->r_frame_rate;
    }

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        return 0.0;
    }

    return av_q2d(frame_rate);
}

}  // namespace

void FfmpegRawVideoSource::initializeOpenCvFormat() {
    const AVCodecParameters* codec_parameters = video_stream_->codecpar;
    const AVPixelFormat pixel_format =
        static_cast<AVPixelFormat>(codec_parameters->format);

    cv::cudacodec::FormatInfo stream_format;
    stream_format.codec = toOpenCvCodec(codec_parameters->codec_id);
    stream_format.chromaFormat = chromaFormatFromPixelFormat(pixel_format);
    stream_format.nBitDepthMinus8 =
        bitDepthMinusEightFromPixelFormat(pixel_format);
    stream_format.nBitDepthChromaMinus8 = stream_format.nBitDepthMinus8;
    stream_format.ulWidth = codec_parameters->width;
    stream_format.ulHeight = codec_parameters->height;
    stream_format.width = codec_parameters->width;
    stream_format.height = codec_parameters->height;
    stream_format.ulMaxWidth = codec_parameters->width;
    stream_format.ulMaxHeight = codec_parameters->height;
    stream_format.displayArea = cv::Rect(
        0,
        0,
        codec_parameters->width,
        codec_parameters->height
    );
    stream_format.valid = false;
    stream_format.fps = streamFramesPerSecond(input_context_, video_stream_);
    stream_format.ulNumDecodeSurfaces = 0;
    stream_format.deinterlaceMode = cv::cudacodec::DeinterlaceMode::Weave;
    stream_format.videoFullRangeFlag =
        codec_parameters->color_range == AVCOL_RANGE_JPEG;
    stream_format.enableHistogram = false;
    stream_format.nCounterBitDepth = 0;
    stream_format.nMaxHistogramBins = 0;

    std::lock_guard<std::mutex> lock(format_mutex_);
    opencv_format_ = stream_format;
}

void FfmpegRawVideoSource::copyCodecExtraDataFromStream() {
    const AVCodecParameters* codec_parameters = video_stream_->codecpar;
    if (codec_parameters->extradata == nullptr ||
        codec_parameters->extradata_size <= 0) {
        codec_extra_data_.release();
        return;
    }

    codec_extra_data_.create(
        1,
        codec_parameters->extradata_size,
        CV_8UC1
    );
    std::memcpy(
        codec_extra_data_.data,
        codec_parameters->extradata,
        static_cast<size_t>(codec_parameters->extradata_size)
    );
}

cv::cudacodec::FormatInfo FfmpegRawVideoSource::format() const {
    std::lock_guard<std::mutex> lock(format_mutex_);
    return opencv_format_;
}

void FfmpegRawVideoSource::updateFormat(
    const cv::cudacodec::FormatInfo& video_format
) {
    std::lock_guard<std::mutex> lock(format_mutex_);
    opencv_format_ = video_format;
    opencv_format_.valid = true;
}

void FfmpegRawVideoSource::getExtraData(cv::Mat& extra_data) const {
    codec_extra_data_.copyTo(extra_data);
}

bool FfmpegRawVideoSource::get(
    int property_id,
    double& property_value
) const {
    const cv::cudacodec::FormatInfo stream_format = format();

    switch (property_id) {
        case cv::CAP_PROP_FRAME_WIDTH:
            property_value = stream_format.width;
            return true;
        case cv::CAP_PROP_FRAME_HEIGHT:
            property_value = stream_format.height;
            return true;
        case cv::CAP_PROP_FPS:
            property_value = stream_format.fps;
            return true;
        default:
            return false;
    }
}

int FfmpegRawVideoSource::getFirstFrameIdx() const {
    return 0;
}
