#include "AvCodecParameters.hpp"
#include "AvOutputFormatContext.hpp"
#include "AvOutputIoContext.hpp"
#include "AvPacket.hpp"
#include "VideoStreamParameters.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

extern "C" {
#include <libavcodec/defs.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

class ScopedTemporaryDirectory {
public:
    ScopedTemporaryDirectory() {
        char directory_path_template[] = "/tmp/tcc-output-io.XXXXXX";
        const char* created_directory_path = mkdtemp(directory_path_template);
        if (!created_directory_path) {
            throw std::runtime_error("Nao foi possivel criar a pasta temporaria do teste.");
        }
        this->directory_path_ = created_directory_path;
    }

    ~ScopedTemporaryDirectory() {
        std::error_code cleanup_error;
        std::filesystem::remove_all(this->directory_path_, cleanup_error);
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept {
        return this->directory_path_;
    }

private:
    std::filesystem::path directory_path_;
};

std::size_t openFileDescriptorCount() {
    std::size_t descriptor_count = 0;
    for (const auto& descriptor_entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)descriptor_entry;
        ++descriptor_count;
    }
    return descriptor_count;
}

std::string readFileContents(const std::filesystem::path& file_path) {
    std::ifstream input_file(file_path, std::ios::binary);
    if (!input_file) {
        throw std::runtime_error("Nao foi possivel ler os bytes do arquivo de teste.");
    }
    std::string contents{std::istreambuf_iterator<char>(input_file), std::istreambuf_iterator<char>()};
    if (input_file.bad()) {
        throw std::runtime_error("A leitura dos bytes de teste falhou.");
    }
    return contents;
}

int main() {
    try {
        std::filesystem::path test_directory_path;
        {
            ScopedTemporaryDirectory test_directory;
            test_directory_path = test_directory.path();
            const std::size_t initial_descriptor_count = openFileDescriptorCount();
            const std::filesystem::path absolute_file_path = test_directory.path() / "saída com espaços.bin";
            const std::string expected_contents{"A\0B", 3};

            AvOutputIoContextPtr output_io_context = openOutputFile(absolute_file_path);
            if (!output_io_context || !output_io_context->write_flag ||
                !std::filesystem::is_regular_file(absolute_file_path) ||
                openFileDescriptorCount() != initial_descriptor_count + 1) {
                throw std::runtime_error("A abertura deve criar um arquivo local para escrita e possuir seu descritor.");
            }
            AVIOContext* opened_output_io_context = output_io_context.get();
            AvOutputIoContextPtr moved_output_io_context = std::move(output_io_context);
            if (output_io_context || moved_output_io_context.get() != opened_output_io_context) {
                throw std::runtime_error("O movimento deve transferir a posse do mesmo contexto de escrita.");
            }
            avio_write(moved_output_io_context.get(),
                       reinterpret_cast<const unsigned char*>(expected_contents.data()),
                       static_cast<int>(expected_contents.size()));
            if (moved_output_io_context->error < 0) {
                throw std::runtime_error("A escrita dos bytes sinteticos falhou.");
            }
            moved_output_io_context.reset();
            if (moved_output_io_context || openFileDescriptorCount() != initial_descriptor_count ||
                readFileContents(absolute_file_path) != expected_contents) {
                throw std::runtime_error("reset() deve fechar o descritor e descarregar os bytes no arquivo.");
            }

            const std::filesystem::path relative_file_path =
                std::filesystem::relative(test_directory.path(), std::filesystem::current_path()) / "relative.bin";
            if (relative_file_path.is_absolute()) {
                throw std::runtime_error("O segundo caminho de teste deve ser relativo.");
            }
            {
                AvOutputIoContextPtr scoped_output_io_context = openOutputFile(relative_file_path);
                if (!scoped_output_io_context ||
                    openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("Um caminho relativo deve abrir o arquivo local correspondente.");
                }
            }
            if (openFileDescriptorCount() != initial_descriptor_count ||
                !std::filesystem::is_regular_file(test_directory.path() / "relative.bin") ||
                std::filesystem::file_size(test_directory.path() / "relative.bin") != 0) {
                throw std::runtime_error("A destruicao por escopo deve fechar o arquivo aberto por caminho relativo.");
            }
            AvOutputIoContextPtr empty_output_io_context;
            empty_output_io_context.reset();
            AvOutputIoContextDeleter{}(nullptr);

            const auto expect_invalid_path_rejection = [](const std::filesystem::path& file_path) {
                const std::size_t previous_descriptor_count = openFileDescriptorCount();
                bool rejected = false;
                try {
                    (void)openOutputFile(file_path);
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                if (!rejected || openFileDescriptorCount() != previous_descriptor_count) {
                    throw std::runtime_error("Um caminho invalido deve ser rejeitado sem manter descritores abertos.");
                }
            };
            expect_invalid_path_rejection(absolute_file_path);
            if (readFileContents(absolute_file_path) != expected_contents) {
                throw std::runtime_error("Recusar um arquivo existente deve preservar seu conteudo.");
            }
            expect_invalid_path_rejection(test_directory.path());

            const std::filesystem::path valid_link_path = test_directory.path() / "existing-target-link";
            std::filesystem::create_symlink(absolute_file_path, valid_link_path);
            expect_invalid_path_rejection(valid_link_path);
            if (!std::filesystem::is_symlink(valid_link_path) ||
                readFileContents(absolute_file_path) != expected_contents) {
                throw std::runtime_error("Recusar um link deve preservar o link e os bytes de seu alvo.");
            }
            const std::filesystem::path absent_link_target = test_directory.path() / "absent-target";
            const std::filesystem::path broken_link_path = test_directory.path() / "absent-target-link";
            std::filesystem::create_symlink(absent_link_target, broken_link_path);
            expect_invalid_path_rejection(broken_link_path);
            if (!std::filesystem::is_symlink(broken_link_path) || std::filesystem::exists(absent_link_target)) {
                throw std::runtime_error("Recusar um link quebrado nao deve criar seu alvo.");
            }

            expect_invalid_path_rejection(std::filesystem::path{});
            std::string path_with_null_byte = (test_directory.path() / "invalid-name").string();
            path_with_null_byte.push_back('\0');
            path_with_null_byte += "trailing-name";
            expect_invalid_path_rejection(std::filesystem::path{path_with_null_byte});
            if (std::filesystem::exists(test_directory.path() / "invalid-name")) {
                throw std::runtime_error("Um byte nulo nao deve causar abertura do prefixo do caminho.");
            }

            const std::filesystem::path missing_parent_path = test_directory.path() / "missing-parent";
            bool missing_parent_rejected = false;
            try {
                (void)openOutputFile(missing_parent_path / "output.bin");
            } catch (const std::filesystem::filesystem_error&) {
                throw;
            } catch (const std::runtime_error& error) {
                missing_parent_rejected = std::string(error.what()).find("erro ") != std::string::npos;
            }
            if (!missing_parent_rejected || std::filesystem::exists(missing_parent_path) ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("Um pai inexistente deve causar erro FFmpeg sem criar diretorios nem manter descritores.");
            }

            constexpr unsigned char h264_codec_extra_data[]{
                0x01, 0x64, 0x00, 0x2a, 0xff, 0xe1, 0x00, 0x13, 0x67, 0x64, 0x00, 0x2a,
                0xac, 0xb4, 0x03, 0xc0, 0x11, 0x3f, 0x2c, 0x94, 0x04, 0x04, 0x04, 0x1b,
                0x42, 0x84, 0xd4, 0x01, 0x00, 0x05, 0x68, 0xee, 0x06, 0xf2, 0xc0
            };
            auto header_codec_parameters = allocateAvCodecParameters();
            header_codec_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            header_codec_parameters->codec_id = AV_CODEC_ID_H264;
            header_codec_parameters->width = 1920;
            header_codec_parameters->height = 1080;
            header_codec_parameters->extradata = static_cast<unsigned char*>(
                av_mallocz(sizeof(h264_codec_extra_data) + AV_INPUT_BUFFER_PADDING_SIZE)
            );
            if (!header_codec_parameters->extradata) {
                throw std::bad_alloc{};
            }
            header_codec_parameters->extradata_size = sizeof(h264_codec_extra_data);
            std::copy(std::begin(h264_codec_extra_data), std::end(h264_codec_extra_data),
                      header_codec_parameters->extradata);
            VideoStreamParameters header_video_stream_parameters{
                *header_codec_parameters, AVRational{1, 90000}
            };
            const auto create_video_output_context = [&] {
                auto context = allocateMp4OutputContext();
                (void)createMp4VideoStream(*context, header_video_stream_parameters);
                return context;
            };
            const auto expect_header_precondition_rejection = [](
                AVFormatContext& context,
                const AvOutputIoContextPtr& io_context
            ) {
                AVIOContext* previous_io_context = context.pb;
                const auto previous_stream_count = context.nb_streams;
                const auto previous_io_position = io_context ? avio_tell(io_context.get()) : 0;
                bool rejected = false;
                try {
                    writeMp4Header(context, io_context);
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                if (!rejected || context.pb != previous_io_context ||
                    context.nb_streams != previous_stream_count ||
                    (io_context && avio_tell(io_context.get()) != previous_io_position)) {
                    throw std::runtime_error("Uma precondicao invalida deve impedir associacao e escrita do cabecalho.");
                }
            };

            const auto header_file_path = test_directory.path() / "header-only.mp4";
            {
                auto header_io_context = openOutputFile(header_file_path);
                AVIOContext* owned_header_io_context = header_io_context.get();
                {
                    auto header_format_context = create_video_output_context();
                    writeMp4Header(*header_format_context, header_io_context);
                    if (!header_io_context || header_io_context.get() != owned_header_io_context ||
                        header_format_context->pb != owned_header_io_context ||
                        header_format_context->streams[0]->time_base.num <= 0 ||
                        header_format_context->streams[0]->time_base.den <= 0 ||
                        avio_tell(header_io_context.get()) <= 0) {
                        throw std::runtime_error("O cabecalho deve usar o I/O emprestado e preservar a base de tempo do muxer.");
                    }
                    expect_header_precondition_rejection(*header_format_context, header_io_context);
                }
                if (!header_io_context || header_io_context.get() != owned_header_io_context ||
                    header_io_context->error < 0 ||
                    openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("Destruir o contexto MP4 nao deve fechar o I/O pertencente a outro proprietario.");
                }
            }
            const std::string header_file_contents = readFileContents(header_file_path);
            if (header_file_contents.size() < 8 || header_file_contents.compare(4, 4, "ftyp") != 0 ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("A saida parcial deve iniciar com ftyp e fechar todos os descritores.");
            }

            {
                auto validation_io_context = openOutputFile(test_directory.path() / "rejected-header.mp4");
                {
                    auto context_without_stream = allocateMp4OutputContext();
                    expect_header_precondition_rejection(*context_without_stream, validation_io_context);
                }
                auto validation_format_context = create_video_output_context();
                AvOutputIoContextPtr missing_io_context;
                expect_header_precondition_rejection(*validation_format_context, missing_io_context);
                validation_format_context->iformat = av_find_input_format("h264");
                if (!validation_format_context->iformat) {
                    throw std::runtime_error("Nao foi possivel preparar o formato de entrada para testar o cabecalho.");
                }
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_format_context->iformat = nullptr;

                AVStream& validation_stream = *validation_format_context->streams[0];
                validation_stream.codecpar->codec_id = AV_CODEC_ID_HEVC;
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_stream.codecpar->codec_id = AV_CODEC_ID_H264;
                validation_stream.codecpar->width = 0;
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_stream.codecpar->width = 1920;
                validation_stream.time_base.num = 0;
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_stream.time_base.num = 1;
                validation_stream.codecpar->extradata_size = -1;
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_stream.codecpar->extradata_size = sizeof(h264_codec_extra_data);

                const int original_write_flag = validation_io_context->write_flag;
                validation_io_context->write_flag = 0;
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_io_context->write_flag = original_write_flag;
                const int original_seekability = validation_io_context->seekable;
                validation_io_context->seekable = 0;
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_io_context->seekable = original_seekability;
                validation_io_context->error = AVERROR(EIO);
                expect_header_precondition_rejection(*validation_format_context, validation_io_context);
                validation_io_context->error = 0;

                AVFormatContext* allocated_non_mp4_context = nullptr;
                const int non_mp4_allocation_result = avformat_alloc_output_context2(
                    &allocated_non_mp4_context, nullptr, "matroska", nullptr
                );
                AvOutputFormatContextPtr non_mp4_context{allocated_non_mp4_context};
                if (non_mp4_allocation_result < 0 || !non_mp4_context) {
                    throw std::runtime_error("Nao foi possivel preparar um contexto nao MP4 para testar o cabecalho.");
                }
                AVStream* non_mp4_stream = avformat_new_stream(non_mp4_context.get(), nullptr);
                if (!non_mp4_stream) {
                    throw std::bad_alloc{};
                }
                if (avcodec_parameters_copy(non_mp4_stream->codecpar, header_codec_parameters.get()) < 0) {
                    throw std::runtime_error("Nao foi possivel preparar os parametros da stream nao MP4.");
                }
                non_mp4_stream->time_base = header_video_stream_parameters.packet_timestamp_time_base;
                expect_header_precondition_rejection(*non_mp4_context, validation_io_context);
            }
            if (openFileDescriptorCount() != initial_descriptor_count ||
                std::filesystem::file_size(test_directory.path() / "rejected-header.mp4") != 0) {
                throw std::runtime_error("Precondicoes invalidas nao devem escrever bytes nem deixar descritores abertos.");
            }

            const auto failed_header_path = test_directory.path() / "failed-header.mp4";
            {
                auto failed_header_io_context = openOutputFile(failed_header_path);
                auto failed_header_format_context = create_video_output_context();
                failed_header_format_context->streams[0]->codecpar->codec_tag = MKTAG('b', 'a', 'd', '!');
                bool muxer_failure_reported = false;
                try {
                    writeMp4Header(*failed_header_format_context, failed_header_io_context);
                } catch (const std::runtime_error& error) {
                    muxer_failure_reported = std::string(error.what()).find("erro -") != std::string::npos;
                }
                if (!muxer_failure_reported ||
                    failed_header_format_context->pb != failed_header_io_context.get()) {
                    throw std::runtime_error("Uma tag incompativel deve causar erro real do muxer com I/O ainda emprestado.");
                }
                failed_header_format_context.reset();
                if (!failed_header_io_context ||
                    openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("O descarte do muxer com falha deve ocorrer antes de fechar seu I/O.");
                }
            }
            if (!std::filesystem::is_regular_file(failed_header_path) ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("Uma falha deve preservar a saida parcial sem deixar recursos abertos.");
            }

            constexpr unsigned char synthetic_avcc_payload[]{0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
            const std::string expected_packet_contents{
                reinterpret_cast<const char*>(synthetic_avcc_payload), sizeof(synthetic_avcc_payload)
            };
            const auto create_prepared_packet = [&](const AVStream& output_stream) {
                auto packet = allocateAvPacket();
                if (av_new_packet(packet.get(), static_cast<int>(sizeof(synthetic_avcc_payload))) < 0) {
                    throw std::runtime_error("Nao foi possivel alocar os bytes do pacote de teste.");
                }
                std::copy(std::begin(synthetic_avcc_payload), std::end(synthetic_avcc_payload), packet->data);
                packet->pts = 90000;
                packet->dts = 90000;
                packet->duration = 3000;
                packet->flags = AV_PKT_FLAG_KEY;
                prepareEncodedPacketForOutput(
                    *packet, header_video_stream_parameters.packet_timestamp_time_base,
                    output_stream.time_base, output_stream.index
                );
                return packet;
            };
            const auto expect_packet_precondition_rejection = [](AVFormatContext& context, AVPacket& packet) {
                const AVPacket previous_packet = packet;
                const std::string previous_bytes = packet.data && packet.size > 0
                    ? std::string(reinterpret_cast<const char*>(packet.data), packet.size)
                    : std::string{};
                AVIOContext* previous_io_context = context.pb;
                const auto previous_io_position = context.pb ? avio_tell(context.pb) : 0;
                bool rejected = false;
                try {
                    writeMp4Packet(context, packet);
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                if (!rejected || packet.buf != previous_packet.buf || packet.data != previous_packet.data ||
                    packet.size != previous_packet.size || packet.pts != previous_packet.pts ||
                    packet.dts != previous_packet.dts || packet.duration != previous_packet.duration ||
                    packet.time_base.num != previous_packet.time_base.num ||
                    packet.time_base.den != previous_packet.time_base.den ||
                    packet.stream_index != previous_packet.stream_index || packet.pos != previous_packet.pos ||
                    packet.flags != previous_packet.flags ||
                    (!previous_bytes.empty() &&
                     previous_bytes != std::string(reinterpret_cast<const char*>(packet.data), packet.size)) ||
                    context.pb != previous_io_context ||
                    (context.pb && avio_tell(context.pb) != previous_io_position)) {
                    throw std::runtime_error("Uma precondicao invalida deve preservar o pacote e nao escrever na saida.");
                }
            };

            const auto single_packet_path = test_directory.path() / "single-packet-partial.mp4";
            {
                auto packet_io_context = openOutputFile(single_packet_path);
                {
                    auto packet_format_context = create_video_output_context();
                    writeMp4Header(*packet_format_context, packet_io_context);
                    AVStream& output_stream = *packet_format_context->streams[0];
                    auto prepared_packet = create_prepared_packet(output_stream);

                    auto context_without_header = create_video_output_context();
                    expect_packet_precondition_rejection(*context_without_header, *prepared_packet);
                    auto context_without_stream = allocateMp4OutputContext();
                    expect_packet_precondition_rejection(*context_without_stream, *prepared_packet);

                    AVFormatContext* allocated_non_mp4_context = nullptr;
                    const int non_mp4_allocation_result = avformat_alloc_output_context2(
                        &allocated_non_mp4_context, nullptr, "matroska", nullptr
                    );
                    AvOutputFormatContextPtr non_mp4_context{allocated_non_mp4_context};
                    if (non_mp4_allocation_result < 0 || !non_mp4_context) {
                        throw std::runtime_error("Nao foi possivel preparar um contexto nao MP4 para testar o pacote.");
                    }
                    expect_packet_precondition_rejection(*non_mp4_context, *prepared_packet);

                    const int original_write_flag = packet_io_context->write_flag;
                    packet_io_context->write_flag = 0;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    packet_io_context->write_flag = original_write_flag;
                    packet_io_context->error = AVERROR(EIO);
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    packet_io_context->error = 0;
                    output_stream.codecpar->codec_id = AV_CODEC_ID_HEVC;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    output_stream.codecpar->codec_id = AV_CODEC_ID_H264;
                    const AVRational output_time_base = output_stream.time_base;
                    output_stream.time_base.num = 0;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    output_stream.time_base = output_time_base;

                    auto empty_packet = allocateAvPacket();
                    expect_packet_precondition_rejection(*packet_format_context, *empty_packet);
                    auto non_refcounted_packet = allocateAvPacket();
                    unsigned char non_refcounted_payload[sizeof(synthetic_avcc_payload)];
                    std::copy(std::begin(synthetic_avcc_payload), std::end(synthetic_avcc_payload),
                              non_refcounted_payload);
                    non_refcounted_packet->data = non_refcounted_payload;
                    non_refcounted_packet->size = sizeof(non_refcounted_payload);
                    non_refcounted_packet->pts = 90000;
                    non_refcounted_packet->dts = 90000;
                    prepareEncodedPacketForOutput(
                        *non_refcounted_packet, header_video_stream_parameters.packet_timestamp_time_base,
                        output_time_base, output_stream.index
                    );
                    expect_packet_precondition_rejection(*packet_format_context, *non_refcounted_packet);

                    unsigned char* original_data = prepared_packet->data;
                    prepared_packet->data = nullptr;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    prepared_packet->data = original_data;
                    const int original_size = prepared_packet->size;
                    prepared_packet->size = 0;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    prepared_packet->size = original_size;
                    prepared_packet->stream_index = output_stream.index + 1;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    prepared_packet->stream_index = output_stream.index;
                    const AVRational invalid_packet_time_bases[]{
                        {0, 1}, {-1, 1}, {1, 0}, {1, -1},
                        {output_time_base.num * 2, output_time_base.den}
                    };
                    for (const AVRational invalid_packet_time_base : invalid_packet_time_bases) {
                        prepared_packet->time_base = invalid_packet_time_base;
                        expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    }
                    prepared_packet->time_base = output_time_base;
                    const auto original_pts = prepared_packet->pts;
                    const auto original_dts = prepared_packet->dts;
                    const auto original_duration = prepared_packet->duration;
                    prepared_packet->pts = AV_NOPTS_VALUE;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    prepared_packet->pts = original_pts;
                    prepared_packet->dts = AV_NOPTS_VALUE;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    prepared_packet->dts = original_dts;
                    prepared_packet->duration = -1;
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    prepared_packet->duration = original_duration;

                    AVPacket* owned_packet = prepared_packet.get();
                    prepared_packet->time_base = {output_time_base.num * 2, output_time_base.den * 2};
                    writeMp4Packet(*packet_format_context, *prepared_packet);
                    if (!prepared_packet || prepared_packet.get() != owned_packet ||
                        prepared_packet->buf || prepared_packet->data || prepared_packet->size != 0 ||
                        prepared_packet->pts != AV_NOPTS_VALUE || prepared_packet->dts != AV_NOPTS_VALUE) {
                        throw std::runtime_error("A escrita deve consumir os dados e preservar o proprietario do AVPacket vazio.");
                    }
                    expect_packet_precondition_rejection(*packet_format_context, *prepared_packet);
                    avio_flush(packet_io_context.get());
                    if (packet_io_context->error < 0) {
                        throw std::runtime_error("O flush exclusivo do teste nao conseguiu escrever os bytes do pacote.");
                    }
                }
                if (!packet_io_context || openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("Liberar o contexto apos escrever o pacote nao deve fechar o I/O emprestado.");
                }
            }
            const std::string single_packet_contents = readFileContents(single_packet_path);
            const auto mdat_position = single_packet_contents.find("mdat");
            if (single_packet_contents.size() < 8 || single_packet_contents.compare(4, 4, "ftyp") != 0 ||
                mdat_position == std::string::npos ||
                single_packet_contents.find(expected_packet_contents, mdat_position + 4) == std::string::npos ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("A saida parcial deve conter os bytes do pacote no mdat sem descritores restantes.");
            }

            {
                auto failed_packet_io_context = openOutputFile(test_directory.path() / "failed-packet-partial.mp4");
                auto failed_packet_format_context = create_video_output_context();
                writeMp4Header(*failed_packet_format_context, failed_packet_io_context);
                auto failed_packet = create_prepared_packet(*failed_packet_format_context->streams[0]);
                AVPacket* owned_failed_packet = failed_packet.get();
                failed_packet->pts = 0;
                failed_packet->dts = 1;
                bool packet_failure_reported = false;
                try {
                    writeMp4Packet(*failed_packet_format_context, *failed_packet);
                } catch (const std::runtime_error& error) {
                    packet_failure_reported = std::string(error.what()).find("erro -") != std::string::npos;
                }
                if (!packet_failure_reported || !failed_packet || failed_packet.get() != owned_failed_packet ||
                    failed_packet->buf || failed_packet->data || failed_packet->size != 0 ||
                    failed_packet->pts != AV_NOPTS_VALUE || failed_packet->dts != AV_NOPTS_VALUE ||
                    failed_packet_format_context->pb != failed_packet_io_context.get()) {
                    throw std::runtime_error("Um erro real do FFmpeg deve ser informado com dados consumidos e AVPacket ainda possuido.");
                }
                failed_packet_format_context.reset();
                if (!failed_packet_io_context || openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("O contexto com erro de pacote deve ser descartado antes de fechar o I/O.");
                }
            }
            if (openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("O teste de erro de pacote nao deve deixar descritores abertos.");
            }

            const auto expect_trailer_precondition_rejection = [](AVFormatContext& context) {
                AVIOContext* previous_io_context = context.pb;
                void* previous_private_data = context.priv_data;
                const auto previous_stream_count = context.nb_streams;
                const auto previous_io_position = context.pb ? avio_tell(context.pb) : 0;
                bool rejected = false;
                try {
                    writeMp4Trailer(context);
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                if (!rejected || context.pb != previous_io_context ||
                    context.priv_data != previous_private_data || context.nb_streams != previous_stream_count ||
                    (context.pb && avio_tell(context.pb) != previous_io_position)) {
                    throw std::runtime_error("Uma precondicao invalida deve impedir o trailer sem alterar o contexto ou o I/O.");
                }
            };
            const auto finalized_file_path = test_directory.path() / "finalized-container.mp4";
            {
                auto trailer_io_context = openOutputFile(finalized_file_path);
                AVIOContext* owned_trailer_io_context = trailer_io_context.get();
                {
                    auto trailer_format_context = create_video_output_context();
                    expect_trailer_precondition_rejection(*trailer_format_context);
                    auto context_without_stream = allocateMp4OutputContext();
                    expect_trailer_precondition_rejection(*context_without_stream);

                    AVFormatContext* allocated_non_mp4_context = nullptr;
                    const int non_mp4_allocation_result = avformat_alloc_output_context2(
                        &allocated_non_mp4_context, nullptr, "matroska", nullptr
                    );
                    AvOutputFormatContextPtr non_mp4_context{allocated_non_mp4_context};
                    if (non_mp4_allocation_result < 0 || !non_mp4_context) {
                        throw std::runtime_error("Nao foi possivel preparar um contexto nao MP4 para testar o trailer.");
                    }
                    expect_trailer_precondition_rejection(*non_mp4_context);

                    writeMp4Header(*trailer_format_context, trailer_io_context);
                    const int original_write_flag = trailer_io_context->write_flag;
                    trailer_io_context->write_flag = 0;
                    expect_trailer_precondition_rejection(*trailer_format_context);
                    trailer_io_context->write_flag = original_write_flag;
                    const int original_seekability = trailer_io_context->seekable;
                    trailer_io_context->seekable = 0;
                    expect_trailer_precondition_rejection(*trailer_format_context);
                    trailer_io_context->seekable = original_seekability;
                    trailer_io_context->error = AVERROR(EIO);
                    expect_trailer_precondition_rejection(*trailer_format_context);
                    trailer_io_context->error = 0;
                    trailer_format_context->streams[0]->codecpar->codec_id = AV_CODEC_ID_HEVC;
                    expect_trailer_precondition_rejection(*trailer_format_context);
                    trailer_format_context->streams[0]->codecpar->codec_id = AV_CODEC_ID_H264;

                    auto packet = create_prepared_packet(*trailer_format_context->streams[0]);
                    writeMp4Packet(*trailer_format_context, *packet);
                    writeMp4Trailer(*trailer_format_context);
                    if (trailer_format_context->pb || !trailer_io_context ||
                        trailer_io_context.get() != owned_trailer_io_context || trailer_io_context->error < 0 ||
                        openFileDescriptorCount() != initial_descriptor_count + 1) {
                        throw std::runtime_error("O trailer deve desassociar pb sem fechar ou transferir a posse do I/O.");
                    }
                    const auto finalized_io_position = avio_tell(trailer_io_context.get());
                    expect_trailer_precondition_rejection(*trailer_format_context);
                    auto packet_after_trailer = create_prepared_packet(*trailer_format_context->streams[0]);
                    expect_packet_precondition_rejection(*trailer_format_context, *packet_after_trailer);
                    if (avio_tell(trailer_io_context.get()) != finalized_io_position) {
                        throw std::runtime_error("Outro trailer ou pacote nao deve escrever apos a finalizacao.");
                    }
                }
                if (!trailer_io_context || trailer_io_context.get() != owned_trailer_io_context ||
                    openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("O I/O finalizado deve continuar aberto ate seu proprietario ser liberado.");
                }
            }
            const std::string finalized_contents = readFileContents(finalized_file_path);
            if (finalized_contents.size() < 8 || finalized_contents.compare(4, 4, "ftyp") != 0 ||
                finalized_contents.find("mdat") == std::string::npos ||
                finalized_contents.find("moov") == std::string::npos ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("O MP4 finalizado deve conter ftyp, mdat e moov sem descritores restantes.");
            }
            {
                AVFormatContext* opened_input_context = nullptr;
                const int open_input_result = avformat_open_input(
                    &opened_input_context, finalized_file_path.c_str(), nullptr, nullptr
                );
                const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
                const std::unique_ptr<AVFormatContext, decltype(close_input)> input_context{
                    opened_input_context, close_input
                };
                if (open_input_result < 0 || !input_context || input_context->nb_streams != 1 ||
                    !input_context->streams || !input_context->streams[0] ||
                    !input_context->streams[0]->codecpar ||
                    input_context->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                    input_context->streams[0]->codecpar->codec_id != AV_CODEC_ID_H264 ||
                    input_context->streams[0]->codecpar->width != 1920 ||
                    input_context->streams[0]->codecpar->height != 1080 ||
                    input_context->streams[0]->nb_frames != 1) {
                    throw std::runtime_error("O MP4 deve reabrir com os metadados H.264 e uma amostra codificada.");
                }
                // O payload sintetico testa o conteiner, nao um frame decodificavel.
                input_context->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
                auto read_packet = allocateAvPacket();
                const int packet_read_result = av_read_frame(input_context.get(), read_packet.get());
                if (packet_read_result < 0 || !read_packet->data ||
                    read_packet->size != static_cast<int>(sizeof(synthetic_avcc_payload)) ||
                    !std::equal(std::begin(synthetic_avcc_payload), std::end(synthetic_avcc_payload), read_packet->data)) {
                    throw std::runtime_error("O demux do MP4 deve devolver os mesmos bytes sinteticos sem decodificacao.");
                }
                av_packet_unref(read_packet.get());
                if (av_read_frame(input_context.get(), read_packet.get()) != AVERROR_EOF) {
                    throw std::runtime_error("O MP4 de teste deve conter somente uma amostra codificada.");
                }
            }
            if (openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("Reabrir o MP4 finalizado nao deve deixar descritores abertos.");
            }

            {
                auto failed_trailer_io_context = openOutputFile(test_directory.path() / "failed-trailer.mp4");
                auto failed_trailer_format_context = create_video_output_context();
                writeMp4Header(*failed_trailer_format_context, failed_trailer_io_context);
                auto packet = create_prepared_packet(*failed_trailer_format_context->streams[0]);
                writeMp4Packet(*failed_trailer_format_context, *packet);
                avio_flush(failed_trailer_io_context.get());
                if (failed_trailer_io_context->error < 0) {
                    throw std::runtime_error("O I/O deve estar sem erro antes da falha injetada de trailer.");
                }
                const auto original_write_packet = failed_trailer_io_context->write_packet;
                failed_trailer_io_context->write_packet = [](void*, std::uint8_t*, int) { return AVERROR(EIO); };
                bool trailer_failure_reported = false;
                try {
                    writeMp4Trailer(*failed_trailer_format_context);
                } catch (const std::runtime_error& error) {
                    failed_trailer_io_context->write_packet = original_write_packet;
                    trailer_failure_reported = std::string(error.what()).find("erro -") != std::string::npos;
                } catch (...) {
                    failed_trailer_io_context->write_packet = original_write_packet;
                    throw;
                }
                failed_trailer_io_context->write_packet = original_write_packet;
                if (!trailer_failure_reported || failed_trailer_format_context->pb ||
                    failed_trailer_io_context->error != AVERROR(EIO) ||
                    openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("A falha injetada deve ser informada com pb desassociado e I/O ainda aberto.");
                }
                expect_trailer_precondition_rejection(*failed_trailer_format_context);
                failed_trailer_format_context.reset();
                if (!failed_trailer_io_context || openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("O contexto com falha de trailer deve ser descartado antes de fechar o I/O.");
                }
            }
            if (openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("A falha injetada de trailer nao deve deixar descritores abertos.");
            }

            const auto create_source_packet = [&](int packet_index) {
                auto packet = allocateAvPacket();
                if (av_new_packet(packet.get(), static_cast<int>(sizeof(synthetic_avcc_payload))) < 0) {
                    throw std::runtime_error("Nao foi possivel alocar o pacote original da colecao de teste.");
                }
                std::copy(std::begin(synthetic_avcc_payload), std::end(synthetic_avcc_payload), packet->data);
                packet->data[5] = static_cast<unsigned char>(0x88 + packet_index);
                packet->pts = packet_index * 3000;
                packet->dts = packet_index * 3000;
                packet->duration = 3000;
                packet->time_base = header_video_stream_parameters.packet_timestamp_time_base;
                packet->stream_index = 5;
                packet->pos = 12345;
                packet->flags = AV_PKT_FLAG_KEY;
                return packet;
            };
            const auto create_source_batches = [&](const std::shared_ptr<const VideoStreamParameters>& parameters) {
                std::vector<EncodedVideoPackets> encoded_packet_batches(3);
                encoded_packet_batches[0].video_stream_parameters = parameters;
                encoded_packet_batches[2].video_stream_parameters = parameters;
                encoded_packet_batches[0].encoded_packets.push_back(create_source_packet(0));
                encoded_packet_batches[0].encoded_packets.push_back(create_source_packet(1));
                encoded_packet_batches[2].encoded_packets.push_back(create_source_packet(2));
                encoded_packet_batches[2].encoded_packets.push_back(create_source_packet(3));
                return encoded_packet_batches;
            };
            const auto expect_batch_rejection = [](
                AVFormatContext& context, std::vector<EncodedVideoPackets> encoded_packet_batches
            ) {
                AVIOContext* previous_io_context = context.pb;
                void* previous_private_data = context.priv_data;
                const auto previous_io_position = context.pb ? avio_tell(context.pb) : 0;
                bool rejected = false;
                try {
                    writeMp4Packets(context, std::move(encoded_packet_batches));
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                if (!rejected || context.pb != previous_io_context || context.priv_data != previous_private_data ||
                    (context.pb && avio_tell(context.pb) != previous_io_position)) {
                    throw std::runtime_error("Uma colecao invalida deve ser rejeitada inteira antes de escrever pacotes.");
                }
            };

            const auto batch_file_path = test_directory.path() / "packet-collections.mp4";
            AVRational batch_output_time_base{};
            auto batch_source_parameters = std::make_shared<const VideoStreamParameters>(
                *header_codec_parameters, header_video_stream_parameters.packet_timestamp_time_base
            );
            const std::weak_ptr<const VideoStreamParameters> transferred_parameters = batch_source_parameters;
            std::vector<AvPacketPtr> source_packets;
            for (int packet_index = 0; packet_index < 4; ++packet_index) {
                source_packets.push_back(create_source_packet(packet_index));
            }
            auto retained_packet = cloneAvPacket(*source_packets.front());
            {
                auto batch_io_context = openOutputFile(batch_file_path);
                auto batch_format_context = allocateMp4OutputContext();
                AVStream& batch_stream = createMp4VideoStream(
                    *batch_format_context, *batch_source_parameters
                );
                batch_stream.time_base = {1, 30000};
                writeMp4Header(*batch_format_context, batch_io_context);
                batch_output_time_base = batch_stream.time_base;
                if (av_cmp_q(batch_output_time_base,
                             header_video_stream_parameters.packet_timestamp_time_base) == 0) {
                    throw std::runtime_error("O teste da colecao deve converter para uma base de tempo diferente da fonte.");
                }
                const auto io_position_before_empty_batches = avio_tell(batch_io_context.get());
                writeMp4Packets(*batch_format_context, {});
                std::vector<EncodedVideoPackets> empty_packet_batches(2);
                writeMp4Packets(*batch_format_context, std::move(empty_packet_batches));
                writeMp4Packets(*batch_format_context, {}, batch_source_parameters);
                writeMp4Packets(*batch_format_context, {}, nullptr);
                if (avio_tell(batch_io_context.get()) != io_position_before_empty_batches) {
                    throw std::runtime_error("Vetores vazios, colecoes vazias e leituras sem pacotes nao devem escrever bytes.");
                }
                writeMp4Packets(
                    *batch_format_context, std::move(source_packets), std::move(batch_source_parameters)
                );
                if (!transferred_parameters.expired() || retained_packet->pts != 0 || retained_packet->dts != 0 ||
                    retained_packet->stream_index != 5 || retained_packet->pos != 12345 ||
                    av_cmp_q(retained_packet->time_base, header_video_stream_parameters.packet_timestamp_time_base) != 0 ||
                    !std::equal(std::begin(synthetic_avcc_payload), std::end(synthetic_avcc_payload), retained_packet->data)) {
                    throw std::runtime_error("A colecao deve liberar seus parametros sem alterar bytes compartilhados ou metadados do clone de teste.");
                }
                writeMp4Trailer(*batch_format_context);
                expect_batch_rejection(*batch_format_context, {});
            }
            if (openFileDescriptorCount() != initial_descriptor_count ||
                av_buffer_get_ref_count(retained_packet->buf) != 1) {
                throw std::runtime_error("A escrita da colecao nao deve manter descritores nem referencias extras ao payload.");
            }
            retained_packet.reset();
            {
                AVFormatContext* opened_batch_input = nullptr;
                const int open_batch_result = avformat_open_input(
                    &opened_batch_input, batch_file_path.c_str(), nullptr, nullptr
                );
                const auto close_input = [](AVFormatContext* input) { avformat_close_input(&input); };
                const std::unique_ptr<AVFormatContext, decltype(close_input)> batch_input{
                    opened_batch_input, close_input
                };
                if (open_batch_result < 0 || !batch_input || batch_input->nb_streams != 1 ||
                    batch_input->streams[0]->nb_frames != 4 ||
                    av_cmp_q(batch_input->streams[0]->time_base, batch_output_time_base) != 0) {
                    throw std::runtime_error("O MP4 da colecao deve reabrir com quatro amostras na base de tempo da saida.");
                }
                // Os quatro payloads sinteticos verificam demux e ordenacao, nao decodificacao.
                batch_input->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
                auto read_packet = allocateAvPacket();
                for (int packet_index = 0; packet_index < 4; ++packet_index) {
                    const int packet_read_result = av_read_frame(batch_input.get(), read_packet.get());
                    const auto expected_timestamp = av_rescale_q(
                        packet_index * 3000, header_video_stream_parameters.packet_timestamp_time_base,
                        batch_output_time_base
                    );
                    const auto expected_duration = av_rescale_q(
                        3000, header_video_stream_parameters.packet_timestamp_time_base, batch_output_time_base
                    );
                    if (packet_read_result < 0 || !read_packet->data ||
                        read_packet->size != static_cast<int>(sizeof(synthetic_avcc_payload)) ||
                        !std::equal(std::begin(synthetic_avcc_payload), std::end(synthetic_avcc_payload) - 1, read_packet->data) ||
                        read_packet->data[5] != static_cast<unsigned char>(0x88 + packet_index) ||
                        read_packet->stream_index != 0 || read_packet->pts != expected_timestamp ||
                        read_packet->dts != expected_timestamp || read_packet->duration != expected_duration) {
                        throw std::runtime_error("Os quatro pacotes devem manter bytes e ordem, com timestamps e duracao convertidos.");
                    }
                    av_packet_unref(read_packet.get());
                }
                if (av_read_frame(batch_input.get(), read_packet.get()) != AVERROR_EOF) {
                    throw std::runtime_error("A colecao deve gravar exatamente os quatro pacotes fornecidos.");
                }
            }
            if (openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("O demux da colecao nao deve deixar descritores abertos.");
            }

            auto validation_source_parameters = std::make_shared<const VideoStreamParameters>(
                *header_codec_parameters, header_video_stream_parameters.packet_timestamp_time_base
            );
            {
                auto rejected_batch_io = openOutputFile(test_directory.path() / "rejected-collections.mp4");
                auto rejected_batch_context = create_video_output_context();
                expect_batch_rejection(*rejected_batch_context, {});
                writeMp4Header(*rejected_batch_context, rejected_batch_io);
                std::vector<AvPacketPtr> packets_without_parameters;
                packets_without_parameters.push_back(create_source_packet(0));
                const auto io_position_before_missing_parameters = avio_tell(rejected_batch_io.get());
                bool missing_parameters_rejected = false;
                try {
                    writeMp4Packets(*rejected_batch_context, std::move(packets_without_parameters), nullptr);
                } catch (const std::invalid_argument&) {
                    missing_parameters_rejected = true;
                }
                if (!missing_parameters_rejected ||
                    avio_tell(rejected_batch_io.get()) != io_position_before_missing_parameters) {
                    throw std::runtime_error("Pacotes com parametros separados ausentes devem ser rejeitados antes de escrever.");
                }
                auto missing_parameters = create_source_batches(validation_source_parameters);
                missing_parameters[2].video_stream_parameters.reset();
                expect_batch_rejection(*rejected_batch_context, std::move(missing_parameters));
                auto different_parameters = create_source_batches(validation_source_parameters);
                different_parameters[2].video_stream_parameters = std::make_shared<const VideoStreamParameters>(
                    *header_codec_parameters, header_video_stream_parameters.packet_timestamp_time_base
                );
                expect_batch_rejection(*rejected_batch_context, std::move(different_parameters));
                auto null_packet = create_source_batches(validation_source_parameters);
                null_packet[2].encoded_packets.back().reset();
                expect_batch_rejection(*rejected_batch_context, std::move(null_packet));
                auto incompatible_time_base = create_source_batches(validation_source_parameters);
                incompatible_time_base[2].encoded_packets.back()->time_base = {1, 1000};
                expect_batch_rejection(*rejected_batch_context, std::move(incompatible_time_base));
                auto unknown_timestamp = create_source_batches(validation_source_parameters);
                unknown_timestamp[2].encoded_packets.back()->pts = AV_NOPTS_VALUE;
                expect_batch_rejection(*rejected_batch_context, std::move(unknown_timestamp));
                auto missing_payload = create_source_batches(validation_source_parameters);
                av_packet_unref(missing_payload[2].encoded_packets.back().get());
                expect_batch_rejection(*rejected_batch_context, std::move(missing_payload));
            }
            const std::string rejected_batch_contents = readFileContents(test_directory.path() / "rejected-collections.mp4");
            if (rejected_batch_contents.find(expected_packet_contents) != std::string::npos ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("Um erro no ultimo grupo nao deve permitir escrever os pacotes anteriores.");
            }

            const auto failed_batch_path = test_directory.path() / "failed-collection-partial.mp4";
            {
                auto failed_batch_io = openOutputFile(failed_batch_path);
                auto failed_batch_context = create_video_output_context();
                writeMp4Header(*failed_batch_context, failed_batch_io);
                auto failed_batches = create_source_batches(validation_source_parameters);
                failed_batches[0].encoded_packets[1]->pts = 0;
                auto retained_unwritten_packet = cloneAvPacket(*failed_batches[2].encoded_packets[0]);
                bool batch_failure_reported = false;
                try {
                    writeMp4Packets(*failed_batch_context, std::move(failed_batches));
                } catch (const std::runtime_error& error) {
                    batch_failure_reported = std::string(error.what()).find("erro -") != std::string::npos;
                }
                if (!batch_failure_reported || av_buffer_get_ref_count(retained_unwritten_packet->buf) != 1) {
                    throw std::runtime_error("O erro no segundo pacote deve interromper a colecao e liberar pacotes nao escritos.");
                }
                failed_batch_context.reset();
                if (!failed_batch_io || openFileDescriptorCount() != initial_descriptor_count + 1) {
                    throw std::runtime_error("O contexto da colecao com erro deve ser descartado antes de fechar o I/O.");
                }
            }
            const std::string failed_batch_contents = readFileContents(failed_batch_path);
            std::string second_packet_contents = expected_packet_contents;
            second_packet_contents[5] = static_cast<char>(0x89);
            std::string third_packet_contents = expected_packet_contents;
            third_packet_contents[5] = static_cast<char>(0x8a);
            std::string fourth_packet_contents = expected_packet_contents;
            fourth_packet_contents[5] = static_cast<char>(0x8b);
            if (failed_batch_contents.find(expected_packet_contents) == std::string::npos ||
                failed_batch_contents.find(second_packet_contents) != std::string::npos ||
                failed_batch_contents.find(third_packet_contents) != std::string::npos ||
                failed_batch_contents.find(fourth_packet_contents) != std::string::npos ||
                openFileDescriptorCount() != initial_descriptor_count) {
                throw std::runtime_error("A saida parcial deve conter somente o primeiro pacote, sem escritas apos a falha.");
            }
        }
        if (std::filesystem::exists(test_directory_path)) {
            throw std::runtime_error("A pasta temporaria do teste nao foi removida: " + test_directory_path.string());
        }
        std::cout << "PASS: abertura local absoluta, relativa e UTF-8; bytes sinteticos preservados apos fechar.\n";
        std::cout << "PASS: transferencia de posse, reset, escopo e proprietario vazio sem descritores restantes.\n";
        std::cout << "PASS: arquivos existentes, diretorios, links e caminhos invalidos recusados sem sobrescrita.\n";
        std::cout << "PASS: pai inexistente causa erro FFmpeg; pasta temporaria propria removida.\n";
        std::cout << "PASS: cabecalho MP4 com ftyp, I/O emprestado e base de tempo positiva, sem pacotes ou trailer.\n";
        std::cout << "PASS: precondicoes e segunda chamada rejeitadas; erro real do muxer tratado com descarte seguro.\n";
        std::cout << "PASS: pacote AVCC sintetico escrito no mdat; dados consumidos e AVPacket ainda possuido, sem trailer.\n";
        std::cout << "PASS: pacotes invalidos preservados; erro real de timestamps consome dados sem vazar descritores.\n";
        std::cout << "PASS: trailer MP4 com moov; mesmos bytes recuperados por demux sem decodificacao.\n";
        std::cout << "PASS: trailer desassocia pb sem fechar I/O; novas escritas e finalizacoes rejeitadas.\n";
        std::cout << "PASS: precondicoes preservam contexto; falha injetada no trailer tratada sem descritores restantes.\n";
        std::cout << "PASS: vetor e parametros separados transferidos por movimento; quatro pacotes com bytes, ordem e timestamps corretos apos demux.\n";
        std::cout << "PASS: vetores e colecoes vazios sem escrita; validacao completa rejeita erro no ultimo grupo antes de escrever.\n";
        std::cout << "PASS: vetor com pacotes e parametros ausentes rejeitado antes de escrever.\n";
        std::cout << "PASS: falha no segundo pacote interrompe escrita e libera a colecao sem descritores ou referencias extras.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
