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
