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

        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
