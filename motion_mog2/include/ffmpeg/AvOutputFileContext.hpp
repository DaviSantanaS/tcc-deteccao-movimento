#pragma once

extern "C" {
#include <libavformat/avio.h>
}

#include <filesystem>
#include <memory>

struct AvOutputFileContextDeleter {
    void operator()(AVIOContext* output_io_context) const noexcept;
};

using AvOutputFileContextPtr =
    std::unique_ptr<AVIOContext, AvOutputFileContextDeleter>;

[[nodiscard]] AvOutputFileContextPtr openOutputFile(
    const std::filesystem::path& output_file_path
);
