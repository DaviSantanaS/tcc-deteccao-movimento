#include "ffmpeg/AvCodecParameters.hpp"
#include "recording/Mp4EventPacketSink.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

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

class ScopedStreamCapture {
public:
    explicit ScopedStreamCapture(std::ostream& output_stream) :
        output_stream_(output_stream),
        previous_buffer_(this->output_stream_.rdbuf(this->captured_stream_.rdbuf())) {
    }

    ~ScopedStreamCapture() {
        this->output_stream_.rdbuf(this->previous_buffer_);
    }

    ScopedStreamCapture(const ScopedStreamCapture&) = delete;
    ScopedStreamCapture& operator=(const ScopedStreamCapture&) = delete;

    std::string getOutput() const {
        return this->captured_stream_.str();
    }

private:
    std::ostream& output_stream_;
    std::ostringstream captured_stream_;
    std::streambuf* previous_buffer_;
};

class ScopedFileWriteFailure {
public:
    ScopedFileWriteFailure() {
        // Os logs capturados ficam em memoria; o limite bloqueia escritas no arquivo do trailer.
        require(getrlimit(RLIMIT_FSIZE, &this->previous_limit_) == 0,
                "Nao foi possivel consultar o limite de tamanho de arquivo.");
        this->previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        require(this->previous_handler_ != SIG_ERR, "Nao foi possivel ignorar SIGXFSZ no teste.");
        struct rlimit restricted_limit = this->previous_limit_;
        restricted_limit.rlim_cur = 0;
        if (setrlimit(RLIMIT_FSIZE, &restricted_limit) != 0) {
            std::signal(SIGXFSZ, this->previous_handler_);
            throw std::runtime_error("Nao foi possivel restringir escritas no teste.");
        }
    }

    ~ScopedFileWriteFailure() {
        (void)setrlimit(RLIMIT_FSIZE, &this->previous_limit_);
        std::signal(SIGXFSZ, this->previous_handler_);
    }

    ScopedFileWriteFailure(const ScopedFileWriteFailure&) = delete;
    ScopedFileWriteFailure& operator=(const ScopedFileWriteFailure&) = delete;

private:
    struct rlimit previous_limit_{};
    void (*previous_handler_)(int) = SIG_DFL;
};

std::size_t countOpenFileDescriptors() {
    std::size_t descriptor_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++descriptor_count;
    }
    return descriptor_count;
}

std::string readFileContents(const std::filesystem::path& path) {
    std::ifstream input_file(path, std::ios::binary);
    require(input_file.is_open(), "Nao foi possivel abrir o arquivo para comparar os bytes.");
    const std::string contents{std::istreambuf_iterator<char>(input_file), std::istreambuf_iterator<char>()};
    require(!input_file.bad(), "Falha ao ler os bytes do arquivo.");
    return contents;
}

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

std::vector<AvPacketPtr> makePackets(int first_packet_index, int packet_count, int keyframe_interval = 60) {
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
        packet->flags = packet_index % keyframe_interval == 0 ? AV_PKT_FLAG_KEY : 0;
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

std::size_t countRecoveredPackets(const std::filesystem::path& recording_path) {
    AVFormatContext* opened_input = nullptr;
    const int input_status = avformat_open_input(&opened_input, recording_path.c_str(), nullptr, nullptr);
    const std::unique_ptr<AVFormatContext, AvInputContextDeleter> input{opened_input};
    require(input_status >= 0 && input && input->nb_streams == 1, "E05-T04: MP4 interrompido deve reabrir.");
    input->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    auto packet = allocateAvPacket();
    std::size_t recovered_count = 0;
    int read_status = 0;
    while ((read_status = av_read_frame(input.get(), packet.get())) >= 0) {
        if (recovered_count == 0) {
            require((packet->flags & AV_PKT_FLAG_KEY) != 0, "E05-T04: primeiro pacote recuperado deve ser keyframe.");
        }
        ++recovered_count;
        av_packet_unref(packet.get());
    }
    require(read_status == AVERROR_EOF, "E05-T04: demux deve terminar sem erro apos o ultimo fragmento completo.");
    return recovered_count;
}

std::size_t measureKillRecovery(
    const std::filesystem::path& recording_path,
    const std::shared_ptr<const VideoStreamParameters>& video_stream_parameters,
    int packet_count,
    std::int64_t fragment_duration_microseconds
) {
    std::cout.flush();
    std::cerr.flush();
    const pid_t child_pid = fork();
    require(child_pid >= 0, "E05-T04: fork falhou.");
    if (child_pid == 0) {
        try {
            auto output_io_context = openOutputFile(recording_path);
            auto mp4_output_context = allocateMp4OutputContext();
            mp4_output_context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
            configureMp4ForInterruptedWriting(*mp4_output_context, fragment_duration_microseconds);
            (void)createMp4VideoStream(*mp4_output_context, *video_stream_parameters);
            writeMp4Header(*mp4_output_context, output_io_context);
            for (int packet_index = 0; packet_index < packet_count; ++packet_index) {
                auto packets = makePackets(packet_index, 1, 600);
                packets.front()->pts = static_cast<std::int64_t>(packet_index) * 1001;
                packets.front()->dts = packets.front()->pts;
                packets.front()->duration = 1001;
                packets.front()->time_base = video_stream_parameters->packet_timestamp_time_base;
                writeMp4Packets(*mp4_output_context, std::move(packets), video_stream_parameters);
            }
            std::raise(SIGKILL);
            std::_Exit(2);
        } catch (...) {
            std::_Exit(3);
        }
    }
    int child_status = 0;
    pid_t waited_pid = -1;
    do {
        waited_pid = waitpid(child_pid, &child_status, 0);
    } while (waited_pid < 0 && errno == EINTR);
    require(waited_pid == child_pid && WIFSIGNALED(child_status) && WTERMSIG(child_status) == SIGKILL,
            "E05-T04: filho deve escrever os pacotes e morrer por SIGKILL.");
    return countRecoveredPackets(recording_path);
}

}  // namespace

int main() {
    static_assert(std::is_nothrow_destructible_v<Mp4EventPacketSink>, "O destrutor do sink deve ser noexcept.");
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

        {
            ScopedTemporaryDirectory directory;
            ScopedStreamCapture output_capture(std::cout);
            std::filesystem::path recording_path;
            {
                Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
                sink.openRecording();
                recording_path = sink.getRecordingDescription();
                sink.writePackets(makePackets(0, 30));
            }
            verifyPackets(recording_path, makePackets(0, 30));
            require(output_capture.getOutput().find("MP4_SAVED_AFTER_ERROR path=") != std::string::npos &&
                        output_capture.getOutput().find("encoded_packets=30\n") != std::string::npos,
                    "E04-T01: destrutor deve finalizar e registrar os trinta pacotes.");
        }
        std::cout << "PASS: E04-T01 destruicao finaliza trinta pacotes e registra MP4_SAVED_AFTER_ERROR.\n";

        {
            ScopedTemporaryDirectory directory;
            ScopedStreamCapture output_capture(std::cout);
            std::filesystem::path recording_path;
            std::string finalized_contents;
            {
                Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
                sink.openRecording();
                recording_path = sink.getRecordingDescription();
                sink.writePackets(makePackets(0, 30));
                sink.finalizeRecording();
                finalized_contents = readFileContents(recording_path);
            }
            require(readFileContents(recording_path) == finalized_contents && output_capture.getOutput().empty(),
                    "E04-T02: destrutor nao deve escrever nem registrar outro trailer apos finalizar.");
        }
        std::cout << "PASS: E04-T02 finalizacao explicita impede trailer duplicado e preserva todos os bytes.\n";

        {
            ScopedTemporaryDirectory directory;
            ScopedStreamCapture output_capture(std::cout);
            std::filesystem::path recording_path;
            bool exception_propagated = false;
            try {
                Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
                sink.openRecording();
                recording_path = sink.getRecordingDescription();
                sink.writePackets(makePackets(0, 30));
                auto invalid_packets = makePackets(30, 2);
                invalid_packets.front()->dts = AV_NOPTS_VALUE;
                sink.writePackets(std::move(invalid_packets));
            } catch (const std::invalid_argument&) {
                exception_propagated = true;
            }
            require(exception_propagated, "E04-T03: erro no segundo lote deve ser propagado.");
            verifyPackets(recording_path, makePackets(0, 30));
            require(output_capture.getOutput().find("MP4_SAVED_AFTER_ERROR path=") != std::string::npos,
                    "E04-T03: desenrolar da pilha deve finalizar o primeiro lote.");
        }
        std::cout << "PASS: E04-T03 erro no segundo lote se propaga e conserva os trinta pacotes anteriores.\n";

        {
            ScopedTemporaryDirectory directory;
            ScopedStreamCapture output_capture(std::cout);
            std::filesystem::path recording_path;
            {
                Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
                sink.openRecording();
                recording_path = sink.getRecordingDescription();
            }
            require(!std::filesystem::exists(recording_path) &&
                        output_capture.getOutput().find("MP4_DISCARDED_EMPTY path=") != std::string::npos,
                    "E04-T04: gravacao vazia deve ser removida e registrada.");
        }
        std::cout << "PASS: E04-T04 destruicao remove gravacao vazia e registra MP4_DISCARDED_EMPTY.\n";

        {
            ScopedStreamCapture output_capture(std::cout);
            ScopedStreamCapture error_capture(std::cerr);
            {
                ScopedTemporaryDirectory directory;
                {
                    Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
                    sink.openRecording();
                    sink.writePackets(makePackets(0, 30));
                    std::filesystem::remove_all(directory.getDirectoryPath());
                }
                require(!std::filesystem::exists(directory.getDirectoryPath()),
                        "E04-T05: diretorio deve ter sido removido antes da destruicao do sink.");
            }
            {
                ScopedTemporaryDirectory directory;
                const std::size_t initial_descriptor_count = countOpenFileDescriptors();
                auto sink = std::make_unique<Mp4EventPacketSink>(directory.getDirectoryPath(), video_stream_parameters);
                sink->openRecording();
                sink->writePackets(makePackets(0, 30));
                {
                    ScopedFileWriteFailure write_failure;
                    sink.reset();
                }
                require(countOpenFileDescriptors() == initial_descriptor_count,
                        "E04-T05: destrutor com falha de trailer deve liberar o descritor.");
            }
            require(error_capture.getOutput().find("[erro] MP4_FINALIZE_FAILED path=") != std::string::npos,
                    "E04-T05: falha real do trailer deve ser registrada sem escapar do destrutor.");
        }
        std::cout << "PASS: E04-T05 destrutor nao lanca com diretorio removido nem com falha real de escrita.\n";

        {
            ScopedTemporaryDirectory directory;
            ScopedStreamCapture output_capture(std::cout);
            ScopedStreamCapture error_capture(std::cerr);
            const std::size_t initial_descriptor_count = countOpenFileDescriptors();
            {
                Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
                sink.openRecording();
                sink.writePackets(makePackets(0, 30));
                bool trailer_failure_propagated = false;
                {
                    ScopedFileWriteFailure write_failure;
                    try {
                        sink.finalizeRecording();
                    } catch (const std::runtime_error& error) {
                        trailer_failure_propagated = std::string(error.what()).find("erro -") != std::string::npos;
                        require(!sink.checkIfRecordingIsOpen() &&
                                    countOpenFileDescriptors() == initial_descriptor_count,
                                "E04-ADENDO: contextos devem estar liberados antes de a excecao chegar ao chamador.");
                    }
                }
                require(trailer_failure_propagated && !sink.checkIfRecordingIsOpen(),
                        "E04-ADENDO: falha real do trailer deve se propagar com gravacao fechada.");
            }
            require(output_capture.getOutput().empty() && error_capture.getOutput().empty(),
                    "E04-ADENDO: destrutor nao deve tentar outro trailer depois da falha explicita.");
        }
        std::cout << "PASS: E04-ADENDO falha do trailer fecha o sink antes de propagar, sem nova tentativa no destrutor.\n";

        {
            ScopedTemporaryDirectory directory;
            auto output_io_context = openOutputFile(directory.getDirectoryPath() / "header-written.mp4");
            auto mp4_output_context = allocateMp4OutputContext();
            (void)createMp4VideoStream(*mp4_output_context, *video_stream_parameters);
            writeMp4Header(*mp4_output_context, output_io_context);
            bool header_rejected = false;
            try {
                configureMp4ForInterruptedWriting(*mp4_output_context, 1'000'000);
            } catch (const std::invalid_argument&) {
                header_rejected = true;
            }
            require(header_rejected, "E05-T02: contexto com cabecalho ja escrito deve ser rejeitado.");
            writeMp4Trailer(*mp4_output_context);
        }
        std::cout << "PASS: E05-T02 contexto com cabecalho escrito rejeitado.\n";

        {
            ScopedTemporaryDirectory directory;
            Mp4EventPacketSink sink(directory.getDirectoryPath(), video_stream_parameters);
            sink.openRecording();
            const std::filesystem::path recording_path = sink.getRecordingDescription();
            sink.writePackets(makePackets(0, 120));
            sink.finalizeRecording();
            verifyPackets(recording_path, makePackets(0, 120));
            AVFormatContext* opened_input = nullptr;
            const int input_status = avformat_open_input(&opened_input, recording_path.c_str(), nullptr, nullptr);
            const std::unique_ptr<AVFormatContext, AvInputContextDeleter> input{opened_input};
            require(input_status >= 0 && input, "E05-T03: arquivo completo deve reabrir.");
            input->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
            auto first_packet = allocateAvPacket();
            require(av_read_frame(input.get(), first_packet.get()) >= 0 && (first_packet->flags & AV_PKT_FLAG_KEY) != 0,
                    "E05-T03: arquivo completo deve iniciar em keyframe.");
        }
        std::cout << "PASS: E05-T03 MP4 completo preserva 120 pacotes, bytes, ordem e keyframe inicial.\n";

        {
            ScopedTemporaryDirectory directory;
            const auto recovery_parameters = std::make_shared<const VideoStreamParameters>(
                *video_stream_parameters->codec_parameters, AVRational{1, 60000}
            );
            for (const int packet_count : {900, 300}) {
                for (const std::int64_t fragment_duration : {std::int64_t{0}, std::int64_t{1'000'000}}) {
                    const auto path = directory.getDirectoryPath() /
                        (std::to_string(packet_count) + "-" + std::to_string(fragment_duration) + ".mp4");
                    const std::size_t recovered_count = measureKillRecovery(
                        path, recovery_parameters, packet_count, fragment_duration
                    );
                    std::cout << "KILL_RECOVERY packets_written=" << packet_count
                              << " fragment_duration_us=" << fragment_duration
                              << " packets_recovered=" << recovered_count << '\n';
                    require(recovered_count <= static_cast<std::size_t>(packet_count),
                            "E05-T04: nao deve recuperar mais pacotes do que os escritos.");
                    if (fragment_duration == 0) {
                        require(recovered_count == static_cast<std::size_t>((packet_count / 600) * 600),
                                "E05-T04: sem duracao maxima, recupera somente GOPs completos.");
                    } else {
                        require(recovered_count >= static_cast<std::size_t>(packet_count - 61),
                                "E05-T04: duracao de 1s deve perder no maximo 61 pacotes.");
                    }
                }
            }
        }
        std::cout << "PASS: E05-T04 quatro mortes por SIGKILL comparam recuperacao por GOP e por duracao.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
