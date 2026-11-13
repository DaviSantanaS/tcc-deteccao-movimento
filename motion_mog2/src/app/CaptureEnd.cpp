#include "app/CaptureEnd.hpp"

CaptureEndReason classifyCaptureEnd(PacketReadEnd packet_read_end, bool source_is_live, bool interrupted_by_signal) {
    if (interrupted_by_signal) {
        return CaptureEndReason::interrupted;
    }
    switch (packet_read_end.reason) {
        case PacketReadEndReason::end_of_stream:
            return source_is_live ? CaptureEndReason::live_source_closed : CaptureEndReason::file_end;
        case PacketReadEndReason::interrupted:
            return CaptureEndReason::interrupted;
        case PacketReadEndReason::packet_preparation_error:
            return CaptureEndReason::packet_error;
        case PacketReadEndReason::read_error:
        case PacketReadEndReason::not_ended:
            return CaptureEndReason::read_error;
    }
    return CaptureEndReason::read_error;
}

int getExitCodeForCaptureEnd(CaptureEndReason capture_end_reason) {
    switch (capture_end_reason) {
        case CaptureEndReason::file_end:
        case CaptureEndReason::interrupted:
            return 0;
        case CaptureEndReason::live_source_closed:
            return 3;
        case CaptureEndReason::read_error:
            return 4;
        case CaptureEndReason::packet_error:
            return 5;
    }
    return 4;
}

const char* getCaptureEndReasonName(CaptureEndReason capture_end_reason) {
    switch (capture_end_reason) {
        case CaptureEndReason::file_end:
            return "file_end";
        case CaptureEndReason::interrupted:
            return "interrupted";
        case CaptureEndReason::live_source_closed:
            return "live_source_closed";
        case CaptureEndReason::read_error:
            return "read_error";
        case CaptureEndReason::packet_error:
            return "packet_error";
    }
    return "read_error";
}
