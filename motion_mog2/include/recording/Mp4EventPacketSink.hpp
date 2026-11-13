#pragma once

#include "ffmpeg/AvMp4OutputContext.hpp"
#include "ffmpeg/AvOutputFileContext.hpp"
#include "ffmpeg/VideoStreamParameters.hpp"
#include "recording/EventPacketSink.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class Mp4EventPacketSink final : public EventPacketSink {
public:
    Mp4EventPacketSink(
        std::filesystem::path output_directory,
        std::shared_ptr<const VideoStreamParameters> video_stream_parameters
    );
    ~Mp4EventPacketSink() noexcept override;

    Mp4EventPacketSink(const Mp4EventPacketSink&) = delete;
    Mp4EventPacketSink& operator=(const Mp4EventPacketSink&) = delete;
    Mp4EventPacketSink(Mp4EventPacketSink&&) = delete;
    Mp4EventPacketSink& operator=(Mp4EventPacketSink&&) = delete;

    void openRecording() override;
    void writePackets(std::vector<AvPacketPtr>&& encoded_packets) override;
    void finalizeRecording() override;
    bool checkIfRecordingIsOpen() const override;
    std::size_t getWrittenPacketCount() const override;
    std::string getRecordingDescription() const override;
    std::size_t getRecordingsStartedWithoutIdrCount() const;

private:
    void releaseRecordingContexts() noexcept;

    // A destruicao inversa libera o contexto MP4 antes de fechar o I/O emprestado.
    AvOutputFileContextPtr output_io_context_;
    AvMp4OutputContextPtr mp4_output_context_;
    std::filesystem::path output_directory_;
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters_;
    std::filesystem::path current_output_file_path_;
    std::size_t written_packet_count_ = 0;
    std::size_t recordings_started_without_idr_count_ = 0;
    bool first_packet_checked_ = false;
    bool header_written_ = false;
};
