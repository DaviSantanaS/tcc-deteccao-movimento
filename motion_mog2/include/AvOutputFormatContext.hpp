#pragma once


extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>

struct VideoStreamParameters;

// O contexto nao fecha AVIOContext nem finaliza uma gravacao.
struct AvOutputFormatContextDeleter {
    void operator()(AVFormatContext* output_context) const noexcept;
};

using AvOutputFormatContextPtr =
    std::unique_ptr<AVFormatContext, AvOutputFormatContextDeleter>;
