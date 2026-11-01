#pragma once

extern "C" {
#include <libavformat/avio.h>
}

#include <filesystem>
#include <memory>

struct AvOutputIoContextDeleter {
    void operator()(AVIOContext* output_io_context) const noexcept;
};

using AvOutputIoContextPtr =
    std::unique_ptr<AVIOContext, AvOutputIoContextDeleter>;

[[nodiscard]] AvOutputIoContextPtr openOutputFile(
    const std::filesystem::path& output_file_path
);
