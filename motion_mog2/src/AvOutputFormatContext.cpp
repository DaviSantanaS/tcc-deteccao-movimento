#include "AvOutputFormatContext.hpp"
#include "AvPacket.hpp"
#include "VideoStreamParameters.hpp"

#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

void AvOutputFormatContextDeleter::operator()(
    AVFormatContext* output_context
) const noexcept {
    if (output_context) {
        avformat_free_context(output_context);
    }
}

AvOutputFormatContextPtr allocateMp4OutputContext() {
    AVFormatContext* allocated_output_context = nullptr;
    const int allocation_result = avformat_alloc_output_context2(
        &allocated_output_context, nullptr, "mp4", nullptr
    );
    AvOutputFormatContextPtr output_context{allocated_output_context};

    if (allocation_result < 0) {
        throw std::runtime_error(
            "Nao foi possivel alocar o contexto MP4: erro " +
            std::to_string(allocation_result)
        );
    }
    if (!output_context) {
        throw std::bad_alloc{};
    }
    return output_context;
}
