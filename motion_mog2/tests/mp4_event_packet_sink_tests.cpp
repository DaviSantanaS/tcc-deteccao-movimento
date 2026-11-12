#include "ffmpeg/AvCodecParameters.hpp"
#include "recording/Mp4EventPacketSink.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename Operation>
void requireLogicError(Operation operation, const std::string& message) {
    try {
        operation();
    } catch (const std::logic_error&) {
        return;
    }
    throw std::runtime_error(message);
}

class ScopedTemporaryDirectory {
public:
    ScopedTemporaryDirectory() {
        char directory_path_template[] = "/tmp/tcc-mp4-sink.XXXXXX";
        const char* created_directory_path = mkdtemp(directory_path_template);
        require(created_directory_path != nullptr, "Nao foi possivel criar o diretorio temporario.");
        this->directory_path_ = created_directory_path;
    }

    ~ScopedTemporaryDirectory() {
        std::error_code cleanup_error;
        std::filesystem::remove_all(this->directory_path_, cleanup_error);
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;

    const std::filesystem::path& getDirectoryPath() const {
        return this->directory_path_;
    }

private:
    std::filesystem::path directory_path_;
};

std::shared_ptr<const VideoStreamParameters> makeStreamParameters() {
    constexpr std::array<unsigned char, 35> codec_extra_data{
        0x01, 0x64, 0x00, 0x2a, 0xff, 0xe1, 0x00, 0x13, 0x67, 0x64, 0x00, 0x2a,
        0xac, 0xb4, 0x03, 0xc0, 0x11, 0x3f, 0x2c, 0x94, 0x04, 0x04, 0x04, 0x1b,
        0x42, 0x84, 0xd4, 0x01, 0x00, 0x05, 0x68, 0xee, 0x06, 0xf2, 0xc0
    };
    auto codec_parameters = allocateAvCodecParameters();
    codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_parameters->codec_id = AV_CODEC_ID_H264;
    codec_parameters->width = 1920;
    codec_parameters->height = 1080;
    codec_parameters->extradata = static_cast<unsigned char*>(
        av_mallocz(codec_extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE)
    );
    if (!codec_parameters->extradata) {
        throw std::bad_alloc{};
    }
    codec_parameters->extradata_size = static_cast<int>(codec_extra_data.size());
    std::copy(codec_extra_data.begin(), codec_extra_data.end(), codec_parameters->extradata);
    return std::make_shared<const VideoStreamParameters>(*codec_parameters, AVRational{1, 90000});
}

std::vector<AvPacketPtr> makePackets(int first_packet_index, int packet_count) {
    std::vector<AvPacketPtr> encoded_packets;
    for (int packet_index = first_packet_index; packet_index < first_packet_index + packet_count; ++packet_index) {
        const std::array<unsigned char, 6> payload{
            0x00, 0x00, 0x00, 0x02, 0x65, static_cast<unsigned char>(0x88 + packet_index)
        };
        auto packet = allocateAvPacket();
        require(av_new_packet(packet.get(), static_cast<int>(payload.size())) >= 0,
                "Nao foi possivel alocar os dados do pacote sintetico.");
        std::copy(payload.begin(), payload.end(), packet->data);
        packet->pts = static_cast<std::int64_t>(packet_index) * 3000;
        packet->dts = packet->pts;
        packet->duration = 3000;
        packet->time_base = {1, 90000};
        packet->stream_index = 5;
        packet->flags = packet_index == 0 ? AV_PKT_FLAG_KEY : 0;
        encoded_packets.push_back(std::move(packet));
    }
    return encoded_packets;
}

struct AvInputContextDeleter {
    void operator()(AVFormatContext* input_context) const noexcept {
        avformat_close_input(&input_context);
    }
};

void verifyPackets(const std::filesystem::path& recording_path, const std::vector<AvPacketPtr>& expected_packets) {
    AVFormatContext* opened_input_context = nullptr;
    const int input_status = avformat_open_input(&opened_input_context, recording_path.c_str(), nullptr, nullptr);
    const std::unique_ptr<AVFormatContext, AvInputContextDeleter> input_context{opened_input_context};
    require(input_status >= 0 && input_context && input_context->nb_streams == 1,
            "O MP4 deve reabrir com uma stream.");
    input_context->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    auto demuxed_packet = allocateAvPacket();
    for (const auto& expected_packet : expected_packets) {
        require(av_read_frame(input_context.get(), demuxed_packet.get()) >= 0 &&
                    demuxed_packet->data && demuxed_packet->size == expected_packet->size &&
                    std::equal(expected_packet->data, expected_packet->data + expected_packet->size,
                               demuxed_packet->data),
                "O demux deve preservar os bytes e a ordem dos pacotes sinteticos.");
        av_packet_unref(demuxed_packet.get());
    }
    require(av_read_frame(input_context.get(), demuxed_packet.get()) == AVERROR_EOF,
            "O MP4 deve conter exatamente a quantidade de pacotes escrita.");
}

}  // namespace

int main() {
    try {
        const auto video_stream_parameters = makeStreamParameters();
        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            EventPacketSink& packet_sink = sink;
            require(!packet_sink.checkIfRecordingIsOpen(), "E02-T01: sink deve iniciar fechado.");
            packet_sink.openRecording();
            require(packet_sink.checkIfRecordingIsOpen(), "E02-T01: abertura deve mudar o estado.");
            const std::filesystem::path recording_path = packet_sink.getRecordingDescription();
            packet_sink.writePackets(makePackets(0, 4));
            packet_sink.finalizeRecording();
            require(!packet_sink.checkIfRecordingIsOpen() && packet_sink.getWrittenPacketCount() == 4,
                    "E02-T01: finalizacao deve fechar e preservar a contagem.");
            verifyPackets(recording_path, makePackets(0, 4));
        }
        std::cout << "PASS: E02-T01 ciclo completo preserva quatro pacotes, bytes e ordem.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            sink.openRecording();
            requireLogicError([&sink] { sink.writePackets(makePackets(1, 2)); },
                              "E02-T02: primeiro lote sem keyframe deve ser rejeitado.");
            require(sink.getWrittenPacketCount() == 0, "E02-T02: lote rejeitado nao deve ser contado.");
            sink.writePackets(makePackets(0, 1));
            sink.finalizeRecording();
        }
        std::cout << "PASS: E02-T02 primeiro lote sem keyframe rejeitado.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            requireLogicError([&sink] { sink.writePackets(makePackets(0, 1)); },
                              "E02-T03: escrever sem abrir deve ser rejeitado.");
            require(std::filesystem::is_empty(directory.getDirectoryPath()),
                    "E02-T03: escrever sem abrir nao deve criar arquivo.");
        }
        std::cout << "PASS: E02-T03 escrita sem abertura rejeitada, sem criar arquivo.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            requireLogicError([&sink] { sink.finalizeRecording(); },
                              "E02-T04: finalizar sem abrir deve ser rejeitado.");
        }
        std::cout << "PASS: E02-T04 finalizacao sem abertura rejeitada.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            sink.openRecording();
            const std::string recording_description = sink.getRecordingDescription();
            requireLogicError([&sink] { sink.openRecording(); },
                              "E02-T05: segunda abertura deve ser rejeitada.");
            require(sink.checkIfRecordingIsOpen() && sink.getRecordingDescription() == recording_description,
                    "E02-T05: segunda abertura deve preservar a gravacao original.");
            sink.writePackets(makePackets(0, 1));
            sink.finalizeRecording();
        }
        std::cout << "PASS: E02-T05 segunda abertura rejeitada.\n";

        {
            ScopedTemporaryDirectory directory;
            bool collision_verified = false;
            // Repete em outro diretorio se o relogio virar o segundo entre as aberturas.
            for (int attempt = 0; attempt < 3 && !collision_verified; ++attempt) {
                const auto output_directory = directory.getDirectoryPath() / std::to_string(attempt);
                std::filesystem::create_directory(output_directory);
                Mp4EventPacketSink sink(output_directory, video_stream_parameters);
                sink.openRecording();
                const std::filesystem::path first_path = sink.getRecordingDescription();
                sink.writePackets(makePackets(0, 1));
                sink.finalizeRecording();
                sink.openRecording();
                require(sink.getWrittenPacketCount() == 0, "E02-T06: nova gravacao deve zerar a contagem.");
                const std::filesystem::path second_path = sink.getRecordingDescription();
                sink.writePackets(makePackets(0, 1));
                sink.finalizeRecording();
                require(first_path != second_path && std::filesystem::is_regular_file(first_path) &&
                            std::filesystem::is_regular_file(second_path),
                        "E02-T06: gravacoes consecutivas devem produzir arquivos distintos.");
                collision_verified = second_path.filename() == first_path.stem().string() + "_001.mp4";
            }
            require(collision_verified, "E02-T06: colisao no mesmo segundo deve usar sufixo _001.");
        }
        std::cout << "PASS: E02-T06 gravacoes no mesmo segundo usam arquivos distintos e sufixo _001.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            sink.openRecording();
            const auto previous_size = std::filesystem::file_size(sink.getRecordingDescription());
            sink.writePackets({});
            require(sink.getWrittenPacketCount() == 0 && sink.checkIfRecordingIsOpen() &&
                        std::filesystem::file_size(sink.getRecordingDescription()) == previous_size,
                    "E02-T07: lote vazio nao deve escrever nem alterar contagem ou estado.");
            sink.writePackets(makePackets(0, 1));
            sink.finalizeRecording();
        }
        std::cout << "PASS: E02-T07 lote vazio aceito sem efeito.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            sink.openRecording();
            sink.writePackets(makePackets(0, 2));
            require(sink.getWrittenPacketCount() == 2, "E02-T08: primeiro lote deve contar dois pacotes.");
            sink.writePackets(makePackets(2, 3));
            require(sink.getWrittenPacketCount() == 5, "E02-T08: os dois lotes devem somar cinco pacotes.");
            sink.finalizeRecording();
        }
        std::cout << "PASS: E02-T08 lotes de dois e tres somam cinco pacotes.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
