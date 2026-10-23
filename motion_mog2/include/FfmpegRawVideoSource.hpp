#pragma once

#include "AvBitstreamFilter.hpp"
#include "AvPacket.hpp"
#include "EncodedVideoPackets.hpp"
#include "VideoStreamParameters.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <opencv2/cudacodec.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class FfmpegRawVideoSource : public cv::cudacodec::RawVideoSource {
public:
    explicit FfmpegRawVideoSource(const std::string& input_url);

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
    std::vector<AvPacketPtr> dequeueEncodedPackets(std::size_t packet_count);
    EncodedVideoPackets takePacketsWithStreamParameters(std::size_t packet_count);
    std::shared_ptr<const VideoStreamParameters> videoStreamParameters() const;

    FfmpegRawVideoSource(const FfmpegRawVideoSource&) = delete;
    FfmpegRawVideoSource& operator=(const FfmpegRawVideoSource&) = delete;
    FfmpegRawVideoSource(FfmpegRawVideoSource&&) = delete;
    FfmpegRawVideoSource& operator=(FfmpegRawVideoSource&&) = delete;

private:
    struct AvFormatContextDeleter {
        void operator()(AVFormatContext* input_context) const noexcept;
    };

    using AvFormatContextPtr =
        std::unique_ptr<AVFormatContext, AvFormatContextDeleter>;

    void openInput(const std::string& input_url);
    void initializeOpenCvFormat();
    void copyCodecExtraDataFromStream();

    AvFormatContextPtr video_source_context;
    AvPacketPtr current_encoded_packet;
    AvPacketPtr current_encoded_packet_for_nvdec;
    AVStream* encoded_video_stream = nullptr;
    int encoded_video_stream_index = -1;
    cv::cudacodec::FormatInfo video_format;
    cv::Mat video_codec_extra_data;
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters;
    AvBitstreamFilterPtr annex_b_filter_for_nvdec;

    std::mutex encoded_packets_queue_mutex;
    std::deque<AvPacketPtr> encoded_packets_queue;
};
