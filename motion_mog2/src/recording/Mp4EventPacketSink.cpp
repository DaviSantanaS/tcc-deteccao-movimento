#include "recording/Mp4EventPacketSink.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::filesystem::path buildRecordingFilePath(const std::filesystem::path& output_directory) {
    const std::time_t recording_start_time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()
    );
    std::tm local_recording_start{};
    if (!localtime_r(&recording_start_time, &local_recording_start)) {
        throw std::runtime_error("Nao foi possivel obter o horario local da gravacao.");
    }
    constexpr std::array<const char*, 12> portuguese_month_names{
        "jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez"
    };
    std::ostringstream recording_file_name;
    recording_file_name << std::put_time(&local_recording_start, "%d_")
                        << portuguese_month_names.at(local_recording_start.tm_mon)
                        << std::put_time(&local_recording_start, "_%y_%H.%M.%S");
    const std::string recording_file_stem = recording_file_name.str();
    std::filesystem::path recording_file_path = output_directory / (recording_file_stem + ".mp4");
    for (std::size_t suffix = 1;
         std::filesystem::symlink_status(recording_file_path).type() != std::filesystem::file_type::not_found;
         ++suffix) {
        std::ostringstream unique_file_name;
        unique_file_name << recording_file_stem << '_' << std::setfill('0') << std::setw(3)
                         << suffix << ".mp4";
        recording_file_path = output_directory / unique_file_name.str();
    }

    return recording_file_path;
}

}  // namespace

Mp4EventPacketSink::Mp4EventPacketSink(
    std::filesystem::path output_directory,
    std::shared_ptr<const VideoStreamParameters> video_stream_parameters
) :
    output_directory_(std::move(output_directory)),
    video_stream_parameters_(std::move(video_stream_parameters)) {
}

void Mp4EventPacketSink::openRecording() {
    if (this->checkIfRecordingIsOpen()) {
        throw std::logic_error("Ja existe uma gravacao MP4 aberta.");
    }
    this->current_output_file_path_ = buildRecordingFilePath(this->output_directory_);
    this->mp4_output_context_ = allocateMp4OutputContext();
    this->mp4_output_context_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    (void)createMp4VideoStream(*this->mp4_output_context_, *this->video_stream_parameters_);
    this->output_io_context_ = openOutputFile(this->current_output_file_path_);
    writeMp4Header(*this->mp4_output_context_, this->output_io_context_);
    this->written_packet_count_ = 0;
}

void Mp4EventPacketSink::writePackets(std::vector<AvPacketPtr>&& encoded_packets) {
    if (!this->checkIfRecordingIsOpen()) {
        throw std::logic_error("Nao existe uma gravacao MP4 aberta para escrita.");
    }
    if (encoded_packets.empty()) {
        return;
    }
    if (this->written_packet_count_ == 0 &&
        (!encoded_packets.front() || (encoded_packets.front()->flags & AV_PKT_FLAG_KEY) == 0)) {
        throw std::logic_error("O primeiro lote do MP4 deve comecar em keyframe.");
    }
    const std::size_t packet_count = encoded_packets.size();
    writeMp4Packets(*this->mp4_output_context_, std::move(encoded_packets), this->video_stream_parameters_);
    this->written_packet_count_ += packet_count;
}

void Mp4EventPacketSink::finalizeRecording() {
    if (!this->checkIfRecordingIsOpen()) {
        throw std::logic_error("Nao existe uma gravacao MP4 aberta para finalizar.");
    }
    writeMp4Trailer(*this->mp4_output_context_);
    this->mp4_output_context_.reset();
    this->output_io_context_.reset();
}

bool Mp4EventPacketSink::checkIfRecordingIsOpen() const {
    return static_cast<bool>(this->mp4_output_context_);
}

std::size_t Mp4EventPacketSink::getWrittenPacketCount() const {
    return this->written_packet_count_;
}

std::string Mp4EventPacketSink::getRecordingDescription() const {
    return this->current_output_file_path_.string();
}
