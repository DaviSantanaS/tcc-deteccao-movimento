#pragma once

#include "AvPacket.hpp"

extern "C" {
#include <libavcodec/bsf.h>
}

#include <memory>
#include <vector>

struct AvBitstreamFilterDeleter {
    void operator()(AVBSFContext* filter_context) const noexcept;
};

using AvBitstreamFilterPtr =
    std::unique_ptr<AVBSFContext, AvBitstreamFilterDeleter>;
