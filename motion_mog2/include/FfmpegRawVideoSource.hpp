#pragma once

#include "AvPacket.hpp"

#include <opencv2/cudacodec.hpp>

extern "C" {
#include <libavformat/avformat.h>
}

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class FfmpegRawVideoSource final : public cv::cudacodec::RawVideoSource {
public:
    explicit FfmpegRawVideoSource(const std::string& rtsp_url);
    ~FfmpegRawVideoSource() override;

    bool getNextPacket(unsigned char** data, size_t* size) override;
    bool lastPacketContainsKeyFrame() const override;

    cv::cudacodec::FormatInfo format() const override;
    void updateFormat(const cv::cudacodec::FormatInfo& video_format) override;
    void getExtraData(cv::Mat& extra_data) const override;
    bool get(int property_id, double& property_value) const override;
    int getFirstFrameIdx() const override;

    std::vector<AvPacketPtr> takePendingPackets(size_t packet_count);
    std::string lastError() const;
    void requestStop() noexcept;

private:
    static int interruptCallback(void* callback_context);

    void openInput(const std::string& rtsp_url);
    void initializeOpenCvFormat();
    void copyCodecExtraDataFromStream();
    void setLastError(const std::string& message);
    void closeInput() noexcept;

    AVFormatContext* input_context_ = nullptr;
    AVStream* video_stream_ = nullptr;
    int video_stream_index_ = -1;
    AvPacketPtr current_packet_;

    mutable std::mutex format_mutex_;
    cv::cudacodec::FormatInfo opencv_format_;
    cv::Mat codec_extra_data_;
    std::vector<uint8_t> first_parser_packet_bytes_;
    bool first_video_packet_ = true;

    std::mutex packet_mutex_;
    std::deque<AvPacketPtr> pending_packets_;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> last_packet_contains_key_frame_{false};
};
