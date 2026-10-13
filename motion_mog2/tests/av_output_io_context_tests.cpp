#include "AvCodecParameters.hpp"
#include "AvOutputFormatContext.hpp"
#include "AvOutputIoContext.hpp"
#include "AvPacket.hpp"
#include "VideoStreamParameters.hpp"

#include <algorithm>
#include <cerrno>
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
        directory_path_ = created_directory_path;
    }

    ~ScopedTemporaryDirectory() {
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory_path_, cleanup_error);
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept {
        return directory_path_;
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
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
