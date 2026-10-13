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
