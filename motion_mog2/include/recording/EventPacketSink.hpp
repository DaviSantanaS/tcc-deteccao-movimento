#pragma once

#include "ffmpeg/AvPacket.hpp"

#include <cstddef>
#include <string>
#include <vector>

class EventPacketSink {
public:
    virtual ~EventPacketSink() = default;
    virtual void openRecording() = 0;
    virtual void writePackets(std::vector<AvPacketPtr>&& encoded_packets) = 0;
    virtual void finalizeRecording() = 0;
    virtual bool checkIfRecordingIsOpen() const = 0;
    virtual std::size_t getWrittenPacketCount() const = 0;
    virtual std::string getRecordingDescription() const = 0;
};
