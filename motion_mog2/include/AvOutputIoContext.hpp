#pragma once

extern "C" {
#include <libavformat/avio.h>
}

#include <filesystem>
#include <memory>

// Fecha contextos abertos por avio_open; erros de fechamento nao sao reportados.
struct AvOutputIoContextDeleter {
    void operator()(AVIOContext* output_io_context) const noexcept;
};

using AvOutputIoContextPtr =
    std::unique_ptr<AVIOContext, AvOutputIoContextDeleter>;

[[nodiscard]] AvOutputIoContextPtr openOutputFile(
    const std::filesystem::path& output_file_path
);
