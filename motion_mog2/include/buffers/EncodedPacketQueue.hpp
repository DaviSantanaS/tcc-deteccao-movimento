#pragma once

#include "ffmpeg/AvPacket.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

// Fila unica de pacotes codificados com uma fronteira:
//
//   frente                     fronteira              fim
//   [ regiao contabilizada    ][ regiao a frente      ]
//
// A regiao contabilizada contem os pacotes ja consumidos pelo decoder,
// isto e, a soma dos N de PROP_NUMBER_OF_RAW_PACKAGES_SINCE_LAST_GRAB.
// A regiao a frente contem pacotes lidos pela thread interna do OpenCV
// que o decoder ainda nao contabilizou.
//
// O produtor so chama pushPacket(). O consumidor avanca a fronteira e
// apara ou retira pacotes somente da regiao contabilizada; a excecao e
// takeAllPackets(), usada no encerramento.
//
// A fronteira e guardada como contador, nao como iterador, porque
// push_back em std::deque invalida iteradores.
class EncodedPacketQueue {
public:
    EncodedPacketQueue() = default;

    void pushPacket(AvPacketPtr encoded_packet);
    void advanceCountedPackets(std::size_t packet_count);

    std::size_t getCountedPacketCount() const;
    std::size_t getQueuedPacketCount() const;

    std::size_t discardCountedPacketsBeforeFirstKeyframe();
    std::size_t trimCountedPackets(
        std::size_t minimum_kept_packet_count,
        std::size_t maximum_counted_packet_count
    );

    [[nodiscard]]
    std::vector<AvPacketPtr> takeCountedPacketsFromLookback(
        std::size_t lookback_packet_count
    );
    [[nodiscard]]
    std::vector<AvPacketPtr> takeCountedPackets();
    [[nodiscard]]
    std::vector<AvPacketPtr> takeAllPackets();

    EncodedPacketQueue(const EncodedPacketQueue&) = delete;
    EncodedPacketQueue& operator=(const EncodedPacketQueue&) = delete;
    EncodedPacketQueue(EncodedPacketQueue&&) = delete;
    EncodedPacketQueue& operator=(EncodedPacketQueue&&) = delete;

private:
    // Os auxiliares abaixo assumem queue_mutex_ ja adquirido.
    bool checkIfKeyframeAt(std::size_t packet_position) const;
    bool findNextCountedKeyframe(
        std::size_t first_position,
        std::size_t& keyframe_position
    ) const;
    std::size_t discardFrontCountedPackets(std::size_t packet_count);
    std::size_t discardLeadingNonKeyframes();
    std::vector<AvPacketPtr> moveFrontPackets(std::size_t packet_count);

    mutable std::mutex queue_mutex_;
    std::deque<AvPacketPtr> encoded_packets_;
    std::size_t counted_packet_count_ = 0;
};
