#include "recording/Mp4EventPacketSink.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
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

Mp4EventPacketSink::~Mp4EventPacketSink() noexcept {
    if (!this->checkIfRecordingIsOpen() || !this->header_written_) {
        return;
    }
    try {
        if (this->written_packet_count_ == 0) {
            this->releaseRecordingContexts();
            std::filesystem::remove(this->current_output_file_path_);
            std::cout << "MP4_DISCARDED_EMPTY path=" << this->current_output_file_path_ << std::endl;
        } else {
            this->finalizeRecording();
            std::cout << "MP4_SAVED_AFTER_ERROR path=" << this->current_output_file_path_
                      << " encoded_packets=" << this->written_packet_count_ << std::endl;
        }
    } catch (const std::exception& error) {
        try {
            std::cerr << "[erro] MP4_FINALIZE_FAILED path=" << this->current_output_file_path_
                      << " reason=" << error.what() << std::endl;
        } catch (...) {
            // Uma falha no proprio log tambem nao pode escapar do destrutor.
        }
    } catch (...) {
        try {
            std::cerr << "[erro] MP4_FINALIZE_FAILED path=" << this->current_output_file_path_
                      << " reason=excecao desconhecida" << std::endl;
        } catch (...) {
        }
    }
}

void Mp4EventPacketSink::openRecording() {
    if (this->checkIfRecordingIsOpen()) {
        throw std::logic_error("Ja existe uma gravacao MP4 aberta.");
    }
    this->header_written_ = false;
    this->current_output_file_path_ = buildRecordingFilePath(this->output_directory_);
    this->mp4_output_context_ = allocateMp4OutputContext();
    this->mp4_output_context_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    // Fecha o fragmento quando chega um pacote com DTS a pelo menos 1 s do
    // primeiro do fragmento, ou quando chega um keyframe. O primeiro fragmento
    // comeca no primeiro pacote do arquivo (keyframe); os demais podem comecar
    // em quadro comum. A duracao limita a perda na interrupcao a cerca de 1 s,
    // independentemente do GOP. A descarga evita reter blocos de 256 KiB no I/O.
    // FFmpeg n6.1: movenc.c:6068-6095; file.c:288-291; doc/muxers.texi, mov/mp4/ismv.
    // https://github.com/FFmpeg/FFmpeg/blob/n6.1/libavformat/movenc.c#L6068-L6095
    // https://github.com/FFmpeg/FFmpeg/blob/n6.1/libavformat/file.c#L288-L291
    // https://github.com/FFmpeg/FFmpeg/blob/n6.1/doc/muxers.texi#L1490-L1539
    constexpr std::int64_t mp4_fragment_duration_microseconds = 1'000'000;
    configureMp4ForInterruptedWriting(*this->mp4_output_context_, mp4_fragment_duration_microseconds);
    (void)createMp4VideoStream(*this->mp4_output_context_, *this->video_stream_parameters_);
    this->output_io_context_ = openOutputFile(this->current_output_file_path_);
    writeMp4Header(*this->mp4_output_context_, this->output_io_context_);
    this->header_written_ = true;
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
    try {
        writeMp4Trailer(*this->mp4_output_context_);
    } catch (...) {
        // Fecha os dois contextos e limpa header_written_ antes do rethrow:
        // o destrutor ve o sink fechado e nao tenta escrever outro trailer.
        this->releaseRecordingContexts();
        throw;
    }
    this->releaseRecordingContexts();
}

void Mp4EventPacketSink::releaseRecordingContexts() noexcept {
    this->mp4_output_context_.reset();
    this->output_io_context_.reset();
    this->header_written_ = false;
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
