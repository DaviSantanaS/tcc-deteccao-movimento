#include "AvOutputIoContext.hpp"

#include <new>
#include <stdexcept>
#include <string>

void AvOutputIoContextDeleter::operator()(
    AVIOContext* output_io_context
) const noexcept {
    if (output_io_context) {
        (void)avio_closep(&output_io_context);
    }
}

AvOutputIoContextPtr openOutputFile(
    const std::filesystem::path& output_file_path
) {
    if (output_file_path.empty() ||
        output_file_path.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("O caminho de saida deve ser nao vazio e sem bytes nulos.");
    }

    const std::filesystem::path absolute_output_file_path =
        std::filesystem::absolute(output_file_path);
    // A consulta nao reserva o caminho; o MVP exige ausencia de criacao concorrente.
    if (std::filesystem::symlink_status(absolute_output_file_path).type() !=
        std::filesystem::file_type::not_found) {
        throw std::invalid_argument("O caminho de saida ja existe.");
    }

    const std::string local_file_url = "file:" + absolute_output_file_path.native();
    AVIOContext* opened_output_io_context = nullptr;
    const int open_output_file_result = avio_open(
        &opened_output_io_context, local_file_url.c_str(), AVIO_FLAG_WRITE
    );
    AvOutputIoContextPtr output_io_context{opened_output_io_context};
    if (open_output_file_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel abrir o arquivo de saida: erro " +
            std::to_string(open_output_file_result)
        );
    }
    if (!output_io_context) {
        throw std::bad_alloc{};
    }
    return output_io_context;
}
