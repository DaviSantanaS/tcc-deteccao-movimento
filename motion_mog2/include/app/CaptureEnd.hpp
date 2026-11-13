#pragma once

#include "input/PacketReadEnd.hpp"

enum class CaptureEndReason { file_end, interrupted, live_source_closed, read_error, packet_error };

CaptureEndReason classifyCaptureEnd(PacketReadEnd packet_read_end, bool source_is_live, bool interrupted_by_signal);
int getExitCodeForCaptureEnd(CaptureEndReason capture_end_reason);
const char* getCaptureEndReasonName(CaptureEndReason capture_end_reason);
