#include "buffers/EncodedPacketQueue.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

void EncodedPacketQueue::pushPacket(AvPacketPtr encoded_packet) {
    if (!encoded_packet) {
        throw std::invalid_argument("A fila nao aceita AVPacket nulo.");
    }

    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    this->encoded_packets_.push_back(std::move(encoded_packet));
}

void EncodedPacketQueue::advanceCountedPackets(std::size_t packet_count) {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);

    const std::size_t uncounted_packet_count =
        this->encoded_packets_.size() - this->counted_packet_count_;

    if (packet_count > uncounted_packet_count) {
        throw std::runtime_error(
            "Fila de AVPacket dessincronizada: esperados " +
            std::to_string(packet_count) + ", disponiveis " +
            std::to_string(uncounted_packet_count) + "."
        );
    }

    this->counted_packet_count_ += packet_count;
}

std::size_t EncodedPacketQueue::getCountedPacketCount() const {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    return this->counted_packet_count_;
}

std::size_t EncodedPacketQueue::getQueuedPacketCount() const {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    return this->encoded_packets_.size();
}

std::size_t EncodedPacketQueue::discardCountedPacketsBeforeFirstKeyframe() {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    return this->discardLeadingNonKeyframes();
}

std::size_t EncodedPacketQueue::trimCountedPackets(
    std::size_t minimum_kept_packet_count,
    std::size_t maximum_counted_packet_count
) {
    if (maximum_counted_packet_count == 0 ||
        maximum_counted_packet_count < minimum_kept_packet_count) {
        throw std::invalid_argument(
            "O teto de pacotes contabilizados deve ser positivo e maior ou igual ao minimo mantido."
        );
    }

    std::lock_guard<std::mutex> lock(this->queue_mutex_);

    // Um MP4 so pode comecar em keyframe; por isso a frente da regiao
    // contabilizada e sempre um keyframe e o corte remove GOPs inteiros.
    std::size_t discarded_packet_count = this->discardLeadingNonKeyframes();
    std::size_t next_keyframe_position = 0;

    while (this->findNextCountedKeyframe(1, next_keyframe_position) &&
           this->counted_packet_count_ - next_keyframe_position >= minimum_kept_packet_count) {
        discarded_packet_count += this->discardFrontCountedPackets(next_keyframe_position);
    }

    // Acima do teto, o limite de memoria prevalece sobre o minimo de
    // pre-evento: descarta o GOP mais antigo mesmo abaixo do minimo e,
    // sem keyframe para cortar, descarta toda a regiao contabilizada.
    while (this->counted_packet_count_ > maximum_counted_packet_count) {
        if (this->findNextCountedKeyframe(1, next_keyframe_position)) {
            discarded_packet_count += this->discardFrontCountedPackets(next_keyframe_position);
        } else {
            discarded_packet_count += this->discardFrontCountedPackets(this->counted_packet_count_);
        }
    }

    return discarded_packet_count;
}

std::vector<AvPacketPtr> EncodedPacketQueue::takeCountedPacketsFromLookback(
    std::size_t lookback_packet_count
) {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);

    const std::size_t target_position =
        this->counted_packet_count_ > lookback_packet_count
            ? this->counted_packet_count_ - lookback_packet_count
            : 0;

    bool keyframe_found = false;
    std::size_t start_keyframe_position = 0;

    const std::size_t last_searched_position =
        std::min(target_position + 1, this->counted_packet_count_);
    for (std::size_t packet_position = 0; packet_position < last_searched_position; ++packet_position) {
        if (this->checkIfKeyframeAt(packet_position)) {
            start_keyframe_position = packet_position;
            keyframe_found = true;
        }
    }

    if (!keyframe_found) {
        keyframe_found = this->findNextCountedKeyframe(target_position + 1, start_keyframe_position);
    }

    if (!keyframe_found) {
        this->discardFrontCountedPackets(this->counted_packet_count_);
        return {};
    }

    this->discardFrontCountedPackets(start_keyframe_position);
    return this->moveFrontPackets(this->counted_packet_count_);
}

std::vector<AvPacketPtr> EncodedPacketQueue::takeCountedPackets() {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    return this->moveFrontPackets(this->counted_packet_count_);
}

std::vector<AvPacketPtr> EncodedPacketQueue::takeCountedPacketsBeforeNextKeyframe(bool& keyframe_found) {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    std::size_t keyframe_position = 0;
    keyframe_found = this->findNextCountedKeyframe(0, keyframe_position);
    return this->moveFrontPackets(keyframe_found ? keyframe_position : this->counted_packet_count_);
}

std::vector<AvPacketPtr> EncodedPacketQueue::takeAllPackets() {
    std::lock_guard<std::mutex> lock(this->queue_mutex_);
    return this->moveFrontPackets(this->encoded_packets_.size());
}

bool EncodedPacketQueue::checkIfKeyframeAt(std::size_t packet_position) const {
    return (this->encoded_packets_[packet_position]->flags & AV_PKT_FLAG_KEY) != 0;
}

bool EncodedPacketQueue::findNextCountedKeyframe(
    std::size_t first_position,
    std::size_t& keyframe_position
) const {
    for (std::size_t packet_position = first_position;
         packet_position < this->counted_packet_count_;
         ++packet_position) {
        if (this->checkIfKeyframeAt(packet_position)) {
            keyframe_position = packet_position;
            return true;
        }
    }

    return false;
}

std::size_t EncodedPacketQueue::discardFrontCountedPackets(std::size_t packet_count) {
    const std::size_t discarded_packet_count = std::min(packet_count, this->counted_packet_count_);

    for (std::size_t packet_index = 0; packet_index < discarded_packet_count; ++packet_index) {
        this->encoded_packets_.pop_front();
    }

    this->counted_packet_count_ -= discarded_packet_count;
    return discarded_packet_count;
}

std::size_t EncodedPacketQueue::discardLeadingNonKeyframes() {
    std::size_t discarded_packet_count = 0;

    while (this->counted_packet_count_ > 0 && !this->checkIfKeyframeAt(0)) {
        discarded_packet_count += this->discardFrontCountedPackets(1);
    }

    return discarded_packet_count;
}

// Move pacotes da frente para o chamador. Pacotes da regiao a frente so
// sao movidos por takeAllPackets(); por isso a fronteira volta a zero ou
// diminui na mesma quantidade movida da regiao contabilizada.
std::vector<AvPacketPtr> EncodedPacketQueue::moveFrontPackets(std::size_t packet_count) {
    std::vector<AvPacketPtr> moved_packets;
    moved_packets.reserve(packet_count);

    for (std::size_t packet_index = 0; packet_index < packet_count; ++packet_index) {
        moved_packets.push_back(std::move(this->encoded_packets_.front()));
        this->encoded_packets_.pop_front();
    }

    this->counted_packet_count_ -= std::min(packet_count, this->counted_packet_count_);
    return moved_packets;
}
