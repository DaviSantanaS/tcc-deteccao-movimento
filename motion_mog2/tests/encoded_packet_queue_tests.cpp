#include "buffers/EncodedPacketQueue.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Pacote sintetico: o pts serve de identificador para conferir a ordem.
AvPacketPtr makeSyntheticPacket(std::int64_t packet_id, bool keyframe) {
    auto packet = allocateAvPacket();
    if (av_new_packet(packet.get(), 1) < 0) {
        throw std::runtime_error("Nao foi possivel alocar o pacote sintetico.");
    }
    packet->data[0] = static_cast<std::uint8_t>(packet_id % 256);
    packet->pts = packet_id;
    packet->dts = packet_id;
    if (keyframe) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }
    return packet;
}

// Empurra pacotes com ids consecutivos; 'K' marca keyframe e 'p' nao.
std::int64_t pushPattern(
    EncodedPacketQueue& encoded_packet_queue,
    const std::string& keyframe_pattern,
    std::int64_t first_packet_id
) {
    std::int64_t packet_id = first_packet_id;
    for (const char packet_kind : keyframe_pattern) {
        encoded_packet_queue.pushPacket(makeSyntheticPacket(packet_id, packet_kind == 'K'));
        ++packet_id;
    }
    return packet_id;
}

std::string makeGopPattern(std::size_t gop_packet_count, std::size_t total_packet_count) {
    std::string keyframe_pattern;
    for (std::size_t packet_index = 0; packet_index < total_packet_count; ++packet_index) {
        keyframe_pattern += (packet_index % gop_packet_count == 0) ? 'K' : 'p';
    }
    return keyframe_pattern;
}

void require(bool condition, const std::string& failure_message) {
    if (!condition) {
        throw std::runtime_error(failure_message);
    }
}

void requireIds(
    const std::vector<AvPacketPtr>& packets,
    std::int64_t first_packet_id,
    std::size_t expected_packet_count,
    const std::string& failure_message
) {
    require(packets.size() == expected_packet_count, failure_message + " (quantidade)");
    for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
        require(
            packets[packet_index] &&
                packets[packet_index]->pts == first_packet_id + static_cast<std::int64_t>(packet_index),
            failure_message + " (ordem)"
        );
    }
}

std::int64_t getFrontId(EncodedPacketQueue& encoded_packet_queue) {
    auto all_packets = encoded_packet_queue.takeAllPackets();
    require(!all_packets.empty(), "Fila vazia ao consultar a frente.");
    return all_packets.front()->pts;
}

}  // namespace

int main() {
    try {
        {
            EncodedPacketQueue encoded_packet_queue;
            bool rejected_null_packet = false;
            try {
                encoded_packet_queue.pushPacket(nullptr);
            } catch (const std::invalid_argument&) {
                rejected_null_packet = true;
            }
            require(rejected_null_packet, "pushPacket deve rejeitar ponteiro nulo.");
            require(encoded_packet_queue.getQueuedPacketCount() == 0, "Pacote nulo nao deve entrar na fila.");
        }
        std::cout << "PASS: pushPacket rejeita ponteiro nulo.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "Kpp", 0);
            encoded_packet_queue.advanceCountedPackets(0);
            require(encoded_packet_queue.getCountedPacketCount() == 0, "Avancar zero nao deve mudar a fronteira.");

            bool rejected_excess = false;
            try {
                encoded_packet_queue.advanceCountedPackets(4);
            } catch (const std::runtime_error&) {
                rejected_excess = true;
            }
            require(rejected_excess, "Avancar alem do disponivel deve lancar runtime_error.");
            require(encoded_packet_queue.getCountedPacketCount() == 0 &&
                        encoded_packet_queue.getQueuedPacketCount() == 3,
                    "Falha no avanco nao deve alterar o estado.");

            encoded_packet_queue.advanceCountedPackets(1);
            encoded_packet_queue.advanceCountedPackets(2);
            require(encoded_packet_queue.getCountedPacketCount() == 3, "Avancos devem somar.");
            require(encoded_packet_queue.getCountedPacketCount() <= encoded_packet_queue.getQueuedPacketCount(),
                    "Contabilizados nao podem exceder o tamanho da fila.");
        }
        std::cout << "PASS: fronteira avanca, soma e rejeita excesso sem alterar estado.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "ppKp", 0);
            encoded_packet_queue.advanceCountedPackets(4);
            require(encoded_packet_queue.discardCountedPacketsBeforeFirstKeyframe() == 2,
                    "Devem ser descartados os dois pacotes antes do keyframe.");
            require(getFrontId(encoded_packet_queue) == 2, "A frente deve ser o keyframe.");
        }
        std::cout << "PASS: descarta pacotes antes do primeiro keyframe.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            std::int64_t next_packet_id = pushPattern(encoded_packet_queue, makeGopPattern(4, 10), 0);
            encoded_packet_queue.advanceCountedPackets(10);
            require(encoded_packet_queue.trimCountedPackets(6, 100) == 4, "Deve descartar exatamente um GOP.");
            require(encoded_packet_queue.getCountedPacketCount() == 6, "Devem restar 6 pacotes.");

            next_packet_id = pushPattern(encoded_packet_queue, "pp", next_packet_id);
            encoded_packet_queue.advanceCountedPackets(2);
            require(encoded_packet_queue.trimCountedPackets(6, 100) == 0,
                    "Cortar outro GOP deixaria menos que o minimo.");
            require(encoded_packet_queue.getCountedPacketCount() == 8, "Devem restar 8 pacotes.");
            requireIds(encoded_packet_queue.takeAllPackets(), 4, 8, "Trim deve manter os ids 4..11");
        }
        std::cout << "PASS: trim corta GOP inteiro sem ficar abaixo do minimo.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "KppKp", 0);
            encoded_packet_queue.advanceCountedPackets(3);
            require(encoded_packet_queue.trimCountedPackets(0, 100) == 0,
                    "Keyframe na regiao a frente nao pode ser usado para cortar.");
            require(encoded_packet_queue.getQueuedPacketCount() == 5 &&
                        encoded_packet_queue.getCountedPacketCount() == 3,
                    "A regiao a frente deve ficar intacta.");
        }
        std::cout << "PASS: trim nunca altera a regiao a frente.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, makeGopPattern(10, 20), 0);
            encoded_packet_queue.advanceCountedPackets(20);
            require(encoded_packet_queue.trimCountedPackets(12, 15) == 10,
                    "Acima do teto deve descartar um GOP mesmo abaixo do minimo.");
            require(encoded_packet_queue.getCountedPacketCount() == 10, "Devem restar 10 pacotes.");
        }
        std::cout << "PASS: teto descarta GOP mesmo abaixo do minimo.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "K" + std::string(20, 'p') + "Kpp", 0);
            encoded_packet_queue.advanceCountedPackets(21);
            require(encoded_packet_queue.trimCountedPackets(5, 10) == 21,
                    "Sem keyframe para cortar, o teto descarta toda a regiao contabilizada.");
            require(encoded_packet_queue.getCountedPacketCount() == 0 &&
                        encoded_packet_queue.getQueuedPacketCount() == 3,
                    "Os 3 pacotes a frente devem permanecer.");
        }
        std::cout << "PASS: teto sem keyframe esvazia so a regiao contabilizada.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, makeGopPattern(4, 15), 0);
            encoded_packet_queue.advanceCountedPackets(12);
            requireIds(encoded_packet_queue.takeCountedPacketsFromLookback(5), 4, 8,
                       "Lookback 5 sobre 12 contabilizados deve comecar no keyframe 4");
            require(encoded_packet_queue.getCountedPacketCount() == 0 &&
                        encoded_packet_queue.getQueuedPacketCount() == 3,
                    "Pacotes a frente devem continuar na fila.");
        }
        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, makeGopPattern(4, 6), 0);
            encoded_packet_queue.advanceCountedPackets(6);
            requireIds(encoded_packet_queue.takeCountedPacketsFromLookback(50), 0, 6,
                       "Lookback maior que a regiao deve comecar no inicio");
        }
        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "pppKp", 0);
            encoded_packet_queue.advanceCountedPackets(5);
            requireIds(encoded_packet_queue.takeCountedPacketsFromLookback(4), 3, 2,
                       "Sem keyframe antes do alvo deve usar o primeiro keyframe depois dele");
        }
        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "pppp", 0);
            encoded_packet_queue.advanceCountedPackets(3);
            require(encoded_packet_queue.takeCountedPacketsFromLookback(2).empty(),
                    "Sem keyframe nenhum deve retornar vazio.");
            require(encoded_packet_queue.getCountedPacketCount() == 0 &&
                        encoded_packet_queue.getQueuedPacketCount() == 1,
                    "Sem keyframe deve descartar so os contabilizados.");
        }
        std::cout << "PASS: lookback comeca no keyframe correto e preserva a regiao a frente.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "KppKp", 0);
            encoded_packet_queue.advanceCountedPackets(3);
            requireIds(encoded_packet_queue.takeCountedPackets(), 0, 3, "takeCountedPackets deve retornar 0..2");
            require(encoded_packet_queue.getCountedPacketCount() == 0 &&
                        encoded_packet_queue.getQueuedPacketCount() == 2,
                    "takeCountedPackets deve manter a regiao a frente.");
            requireIds(encoded_packet_queue.takeAllPackets(), 3, 2, "takeAllPackets deve retornar 3..4");
            require(encoded_packet_queue.getQueuedPacketCount() == 0, "A fila deve ficar vazia.");
        }
        std::cout << "PASS: takeCountedPackets e takeAllPackets preservam a ordem.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "ppKpp", 0);
            encoded_packet_queue.advanceCountedPackets(4);
            bool keyframe_found = false;
            requireIds(encoded_packet_queue.takeCountedPacketsBeforeNextKeyframe(keyframe_found), 0, 2,
                       "Deve mover somente os pacotes 0 e 1 antes do keyframe");
            require(keyframe_found && encoded_packet_queue.getCountedPacketCount() == 2 &&
                        encoded_packet_queue.getQueuedPacketCount() == 3, "Keyframe e fronteira devem permanecer.");
            auto counted_packets = encoded_packet_queue.takeCountedPackets();
            requireIds(counted_packets, 2, 2, "Os contabilizados devem recomecar no keyframe 2");
            require((counted_packets.front()->flags & AV_PKT_FLAG_KEY) != 0, "A frente deve continuar keyframe.");
            requireIds(encoded_packet_queue.takeAllPackets(), 4, 1, "O pacote a frente deve permanecer intacto");
        }
        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "Kp", 0);
            encoded_packet_queue.advanceCountedPackets(2);
            bool keyframe_found = false;
            require(encoded_packet_queue.takeCountedPacketsBeforeNextKeyframe(keyframe_found).empty(),
                    "Keyframe na posicao zero deve produzir lote vazio.");
            require(keyframe_found && encoded_packet_queue.getCountedPacketCount() == 2,
                    "Keyframe na posicao zero deve permanecer contabilizado.");
            requireIds(encoded_packet_queue.takeCountedPackets(), 0, 2, "Nenhum pacote deve ter sido retirado");
        }
        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, "ppK", 0);
            encoded_packet_queue.advanceCountedPackets(2);
            bool keyframe_found = true;
            requireIds(encoded_packet_queue.takeCountedPacketsBeforeNextKeyframe(keyframe_found), 0, 2,
                       "Sem keyframe contabilizado deve mover toda a regiao contabilizada");
            require(!keyframe_found && encoded_packet_queue.getCountedPacketCount() == 0 &&
                        encoded_packet_queue.getQueuedPacketCount() == 1,
                    "Keyframe a frente nao deve ser usado nem retirado.");
            auto ahead_packets = encoded_packet_queue.takeAllPackets();
            requireIds(ahead_packets, 2, 1, "O keyframe a frente deve permanecer");
            require((ahead_packets.front()->flags & AV_PKT_FLAG_KEY) != 0, "Flag do keyframe a frente preservada.");
        }
        std::cout << "PASS: takeCountedPacketsBeforeNextKeyframe preserva o keyframe e a regiao a frente.\n";

        {
            EncodedPacketQueue encoded_packet_queue;
            pushPattern(encoded_packet_queue, makeGopPattern(2, 6), 0);
            encoded_packet_queue.advanceCountedPackets(6);
            for (const auto& [minimum_kept, maximum_counted] :
                 std::vector<std::pair<std::size_t, std::size_t>>{{0, 0}, {5, 4}}) {
                bool rejected_limits = false;
                try {
                    (void)encoded_packet_queue.trimCountedPackets(minimum_kept, maximum_counted);
                } catch (const std::invalid_argument&) {
                    rejected_limits = true;
                }
                require(rejected_limits, "Limites invalidos devem lancar invalid_argument.");
                require(encoded_packet_queue.getCountedPacketCount() == 6,
                        "Limites invalidos nao devem alterar o estado.");
            }
        }
        std::cout << "PASS: trim rejeita limites invalidos sem alterar estado.\n";

        {
            constexpr std::int64_t produced_packet_count = 100000;
            EncodedPacketQueue encoded_packet_queue;
            std::atomic<bool> producer_finished{false};

            std::thread producer_thread([&encoded_packet_queue, &producer_finished]() {
                for (std::int64_t packet_id = 0; packet_id < produced_packet_count; ++packet_id) {
                    encoded_packet_queue.pushPacket(makeSyntheticPacket(packet_id, packet_id % 30 == 0));
                }
                producer_finished = true;
            });

            std::vector<std::int64_t> returned_packet_ids;
            std::size_t discarded_packet_count = 0;
            const auto collect_packets = [&returned_packet_ids](std::vector<AvPacketPtr> packets) {
                for (const auto& packet : packets) {
                    returned_packet_ids.push_back(packet->pts);
                }
            };

            for (std::size_t iteration = 0;; ++iteration) {
                const bool finished_before_reading = producer_finished;
                const std::size_t uncounted_packet_count =
                    encoded_packet_queue.getQueuedPacketCount() - encoded_packet_queue.getCountedPacketCount();
                if (finished_before_reading && uncounted_packet_count == 0) {
                    break;
                }
                encoded_packet_queue.advanceCountedPackets(
                    std::min<std::size_t>(uncounted_packet_count, 1 + iteration % 7)
                );
                if (iteration % 5 == 0) {
                    discarded_packet_count += encoded_packet_queue.trimCountedPackets(60, 200);
                }
                if (iteration % 11 == 0) {
                    collect_packets(encoded_packet_queue.takeCountedPackets());
                }
            }

            producer_thread.join();
            collect_packets(encoded_packet_queue.takeAllPackets());

            require(returned_packet_ids.size() + discarded_packet_count ==
                        static_cast<std::size_t>(produced_packet_count),
                    "Retornados + descartados deve ser igual ao produzido.");
            for (std::size_t packet_index = 1; packet_index < returned_packet_ids.size(); ++packet_index) {
                require(returned_packet_ids[packet_index - 1] < returned_packet_ids[packet_index],
                        "Ids retornados devem ser estritamente crescentes.");
            }
        }
        std::cout << "PASS: produtor e consumidor concorrentes sem perda nem duplicacao.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
