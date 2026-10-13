#include "AvBitstreamFilter.hpp"

extern "C" {
#include <libavutil/error.h>
}

#include <new>
#include <stdexcept>
#include <string>
#include <utility>

void AvBitstreamFilterDeleter::operator()(AVBSFContext* filter_context) const noexcept {
    av_bsf_free(&filter_context);
}
