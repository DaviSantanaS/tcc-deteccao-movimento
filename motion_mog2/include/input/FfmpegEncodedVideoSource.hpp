#pragma once

#include "buffers/EncodedPacketQueue.hpp"
#include "ffmpeg/AvBitstreamFilter.hpp"
#include "ffmpeg/AvPacket.hpp"
#include "ffmpeg/EncodedVideoPackets.hpp"
#include "ffmpeg/VideoStreamParameters.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <opencv2/cudacodec.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class FfmpegEncodedVideoSource : public cv::cudacodec::RawVideoSource {
public:
    explicit FfmpegEncodedVideoSource(const std::string& input_source);

    cv::cudacodec::FormatInfo format() const override;
    void updateFormat(const cv::cudacodec::FormatInfo& updated_video_format) override;
    void getExtraData(cv::Mat& extra_data) const override;
    bool getNextPacket(
        unsigned char** encoded_packet_data_for_nvdec,
        std::size_t* encoded_packet_size_bytes_for_nvdec
    ) override;
    bool lastPacketContainsKeyFrame() const override;
    bool get(int property_id, double& property_value) const override;
    int getFirstFrameIdx() const override;

    [[nodiscard]]
    std::vector<AvPacketPtr> dequeueEncodedPackets(std::size_t packet_count);

    [[nodiscard]]
    EncodedVideoPackets dequeueEncodedPacketsWithStreamParameters(std::size_t packet_count);

    std::shared_ptr<const VideoStreamParameters> getVideoStreamParameters() const;

    FfmpegEncodedVideoSource(const FfmpegEncodedVideoSource&) = delete;
    FfmpegEncodedVideoSource& operator=(const FfmpegEncodedVideoSource&) = delete;
    FfmpegEncodedVideoSource(FfmpegEncodedVideoSource&&) = delete;
    FfmpegEncodedVideoSource& operator=(FfmpegEncodedVideoSource&&) = delete;

private:
    struct AvFormatContextDeleter {
        void operator()(AVFormatContext* input_context) const noexcept;
    };

    using AvFormatContextPtr =
        std::unique_ptr<AVFormatContext, AvFormatContextDeleter>;

    void openInputAndSelectVideoStream(const std::string& input_source);
    void initializeVideoFormat();
    void copyVideoCodecExtraData();

    AvFormatContextPtr video_source_context_;
    AvPacketPtr current_encoded_packet_;
    AvPacketPtr current_encoded_packet_for_nvdec_;
    AVStream* encoded_video_stream_ = nullptr;
    int encoded_video_stream_index_ = -1;
    cv::cudacodec::FormatInfo video_format_;
    cv::Mat video_codec_extra_data_;
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters_;
    AvBitstreamFilterPtr annex_b_filter_for_nvdec_;

    EncodedPacketQueue encoded_packet_queue_;
};
