#pragma once

#include "ffmpeg/AvPacket.hpp"

#include <cstddef>
#include <deque>
#include <mutex>

// Unica fila de pacotes codificados do projeto. Recebe os clones dos
// AVPacket originais na thread interna do OpenCV (getNextPacket) e os
// entrega, em ordem FIFO, a thread do main, que os grava no MP4.
class EncodedVideoBuffer {
public:
    // Chamado pela thread interna do OpenCV, dentro de getNextPacket.
    void pushPacket(AvPacketPtr encoded_packet);

    // Chamado pela thread do main. Devolve nullptr quando o buffer esta vazio.
    [[nodiscard]]
    AvPacketPtr tryPopPacket();

    [[nodiscard]]
    std::size_t getPacketCount() const;

private:
    // Protege a fila: o produtor e o consumidor rodam em threads diferentes.
    mutable std::mutex encoded_packets_mutex_;
    std::deque<AvPacketPtr> encoded_packets_;
};
