#include "recording/EventRecorder.hpp"
#include "recording/PacketTimestampCheck.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FakeRecording {
    std::vector<std::int64_t> packet_ids;
    bool starts_with_keyframe = false;
    bool finalized = false;
};

class FakeEventPacketSink final : public EventPacketSink {
public:
    void openRecording() override {
        require(!this->recording_open_, "Sink falso ja aberto.");
        this->recordings_.emplace_back();
        this->recording_open_ = true;
    }

    void writePackets(std::vector<AvPacketPtr>&& encoded_packets) override {
        require(this->recording_open_, "Escrita no sink falso fechado.");
        auto& recording = this->recordings_.back();
        if (recording.packet_ids.empty() && !encoded_packets.empty()) {
            recording.starts_with_keyframe = (encoded_packets.front()->flags & AV_PKT_FLAG_KEY) != 0;
            require(recording.starts_with_keyframe, "Gravacao deve comecar em keyframe.");
        }
        for (const auto& packet : encoded_packets) {
            recording.packet_ids.push_back(packet->pts);
        }
    }

    void finalizeRecording() override {
        require(this->recording_open_, "Finalizacao do sink falso fechado.");
        this->recordings_.back().finalized = true;
        this->recording_open_ = false;
    }

    bool checkIfRecordingIsOpen() const override {
        return this->recording_open_;
    }

    std::size_t getWrittenPacketCount() const override {
        return this->recordings_.empty() ? 0 : this->recordings_.back().packet_ids.size();
    }

    std::string getRecordingDescription() const override {
        return "recording-" + std::to_string(this->recordings_.size()) + ".mp4";
    }

    const std::vector<FakeRecording>& getRecordings() const {
        return this->recordings_;
    }

private:
    std::vector<FakeRecording> recordings_;
    bool recording_open_ = false;
};

AvPacketPtr makePacket(std::int64_t packet_id, bool keyframe) {
    auto packet = allocateAvPacket();
    require(av_new_packet(packet.get(), 1) >= 0, "Falha ao alocar pacote sintetico.");
    packet->data[0] = static_cast<std::uint8_t>(packet_id % 256);
    packet->pts = packet_id;
    packet->dts = packet_id;
    packet->flags = keyframe ? AV_PKT_FLAG_KEY : 0;
    return packet;
}

MotionState motionEdges(bool started, bool ended) {
    MotionState motion_state;
    motion_state.started = started;
    motion_state.ended = ended;
    return motion_state;
}

struct Scenario {
    EncodedPacketQueue encoded_packet_queue;
    FakeEventPacketSink packet_sink;
    std::ostringstream event_log;
    EventRecorder event_recorder;

    explicit Scenario(EventRecorderConfig recorder_config) :
        event_recorder(this->encoded_packet_queue, this->packet_sink, recorder_config, this->event_log) {
    }

    void read(std::int64_t packet_id, bool keyframe, bool started = false, bool ended = false) {
        this->encoded_packet_queue.pushPacket(makePacket(packet_id, keyframe));
        this->encoded_packet_queue.advanceCountedPackets(1);
        this->event_recorder.processDecodedFrame(motionEdges(started, ended), static_cast<std::uint64_t>(packet_id));
    }

    void finish() {
        this->event_recorder.finishCapture(this->encoded_packet_queue.takeAllPackets());
    }
};

void appendCountedPackets(
    Scenario& scenario,
    std::int64_t first_id,
    std::int64_t last_id,
    std::int64_t invalid_id = -1,
    PacketTimestampIssue issue = PacketTimestampIssue::none,
    std::int64_t additional_keyframe_id = -1
) {
    for (std::int64_t packet_id = first_id; packet_id <= last_id; ++packet_id) {
        auto packet = makePacket(packet_id, packet_id % 60 == 0 || packet_id == additional_keyframe_id);
        if (packet_id == invalid_id) {
            if (issue == PacketTimestampIssue::missing_timestamp) {
                packet->pts = AV_NOPTS_VALUE;
                packet->dts = AV_NOPTS_VALUE;
            } else if (issue == PacketTimestampIssue::pts_before_dts) {
                packet->pts = packet->dts - 1;
            } else if (issue == PacketTimestampIssue::non_increasing_dts) {
                packet->dts = packet_id - 1;
            }
        }
        scenario.encoded_packet_queue.pushPacket(std::move(packet));
    }
    scenario.encoded_packet_queue.advanceCountedPackets(static_cast<std::size_t>(last_id - first_id + 1));
}

void requireTimestampLog(
    const Scenario& scenario,
    const char* reason,
    std::uint64_t frame_index,
    std::size_t dropped_packets,
    bool resumed
) {
    const std::string log = scenario.event_log.str();
    const std::string marker = "EVENT_INVALID_TIMESTAMP ";
    const std::string expected_line = marker + "reason=" + reason + " frame=" + std::to_string(frame_index) +
        " dropped_packets=" + std::to_string(dropped_packets) + " resumed=" + (resumed ? "yes" : "no") + '\n';
    require(log.find(expected_line) != std::string::npos && log.find(marker) == log.rfind(marker),
            "Deve haver uma unica linha de timestamp invalido com os campos esperados por lote.");
}

void requireRange(const FakeRecording& recording, std::int64_t first, std::int64_t last) {
    require(recording.finalized && recording.starts_with_keyframe, "Gravacao deve ser finalizada e iniciar em keyframe.");
    require(recording.packet_ids.size() == static_cast<std::size_t>(last - first + 1), "Quantidade inesperada no trecho.");
    for (std::size_t index = 0; index < recording.packet_ids.size(); ++index) {
        require(recording.packet_ids[index] == first + static_cast<std::int64_t>(index),
                "Identificadores do trecho devem ser exatos e consecutivos.");
    }
}

std::vector<FakeRecording> simulateBaseline(std::size_t post_event_packet_count, bool irregular = false) {
    Scenario scenario({333, 1532, post_event_packet_count});
    constexpr std::array<std::uint64_t, 5> starts{118, 404, 875, 929, 1014};
    constexpr std::array<std::uint64_t, 5> ends{205, 464, 921, 972, 1100};
    constexpr std::array<std::size_t, 8> advances{4, 0, 0, 3, 0, 0, 1, 0};
    for (std::int64_t packet_id = 0; packet_id < 1181; ++packet_id) {
        scenario.encoded_packet_queue.pushPacket(makePacket(packet_id, packet_id % 60 == 0));
    }
    std::size_t counted_total = irregular ? 29 : 0;
    scenario.encoded_packet_queue.advanceCountedPackets(counted_total);
    for (std::uint64_t frame_index = 0; frame_index < 1181; ++frame_index) {
        const std::size_t advance = std::min<std::size_t>(
            irregular ? advances[frame_index % advances.size()] : 1, 1181 - counted_total
        );
        scenario.encoded_packet_queue.advanceCountedPackets(advance);
        counted_total += advance;
        require(counted_total >= frame_index + 1 && counted_total - (frame_index + 1) <= 32,
                "Fronteira sintetica deve ficar entre o quadro atual e 32 pacotes a frente.");
        scenario.event_recorder.processDecodedFrame(
            motionEdges(std::find(starts.begin(), starts.end(), frame_index) != starts.end(),
                        std::find(ends.begin(), ends.end(), frame_index) != ends.end()), frame_index
        );
    }
    scenario.finish();
    require(scenario.event_recorder.getCompletedRecordingCount() == scenario.packet_sink.getRecordings().size(),
            "Contagem de gravacoes concluidas incorreta.");
    return scenario.packet_sink.getRecordings();
}

}  // namespace

int main() {
    try {
        {
            const auto recordings = simulateBaseline(300);
            require(recordings.size() == 2, "E03-T01: baseline deve produzir duas gravacoes.");
            requireRange(recordings[0], 0, 779);
            requireRange(recordings[1], 780, 1180);
        }
        std::cout << "PASS: E03-T01 baseline com pos-evento 300 preserva [0,779] e [780,1180].\n";

        {
            const auto recordings = simulateBaseline(0);
            require(recordings.size() == 3, "E03-T02: baseline sem pos-evento deve produzir tres gravacoes.");
            requireRange(recordings[0], 0, 239);
            requireRange(recordings[1], 240, 479);
            requireRange(recordings[2], 540, 1139);
        }
        std::cout << "PASS: E03-T02 baseline sem pos-evento preserva os tres trechos esperados.\n";

        {
            Scenario scenario({1, 100, 0});
            scenario.read(0, false, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::waiting_keyframe &&
                        scenario.event_log.str().find("EVENT_WAITING_KEYFRAME\n") != std::string::npos,
                    "E03-T03: inicio sem keyframe deve esperar.");
            scenario.read(1, false);
            scenario.read(2, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::recording,
                    "E03-T03: keyframe deve abrir gravacao.");
            scenario.finish();
            requireRange(scenario.packet_sink.getRecordings().at(0), 2, 2);
        }
        std::cout << "PASS: E03-T03 espera por keyframe e abre a partir dele.\n";

        {
            Scenario scenario({1, 100, 0});
            scenario.read(0, false, true);
            scenario.read(1, false, false, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::idle &&
                        scenario.packet_sink.getRecordings().empty() &&
                        scenario.event_log.str().find("EVENT_DISCARDED_NO_KEYFRAME\n") != std::string::npos,
                    "E03-T04: fim antes do keyframe deve descartar o evento.");
            Scenario same_read({1, 100, 0});
            same_read.read(0, false, true);
            same_read.read(1, true, false, true);
            require(same_read.event_recorder.getRecordingState() == RecordingState::closing_at_keyframe &&
                        same_read.event_log.str().find("EVENT_ENDED frame=1\n") != std::string::npos,
                    "E03-T04: ended na leitura que abriu deve iniciar o fechamento.");
            same_read.finish();
            requireRange(same_read.packet_sink.getRecordings().at(0), 1, 1);
        }
        std::cout << "PASS: E03-T04 descarta sem keyframe e trata fim na leitura de abertura.\n";

        {
            Scenario scenario({1, 100, 3});
            scenario.read(0, true, true);
            scenario.read(1, false, false, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::post_event,
                    "E03-T05: preparo deve entrar em pos-evento.");
            scenario.read(2, false, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::recording &&
                        scenario.event_log.str().find("EVENT_RESUMED frame=2\n") != std::string::npos,
                    "E03-T05: retomada deve continuar a mesma gravacao.");
            scenario.finish();
            require(scenario.packet_sink.getRecordings().size() == 1, "E03-T05: nao deve abrir outro arquivo.");
            requireRange(scenario.packet_sink.getRecordings()[0], 0, 2);
        }
        std::cout << "PASS: E03-T05 retomada durante pos-evento conserva a gravacao.\n";

        {
            Scenario scenario({1, 100, 1});
            scenario.read(0, true, true);
            scenario.read(1, false, false, true);
            scenario.read(2, false);
            require(scenario.event_recorder.getRecordingState() == RecordingState::closing_at_keyframe,
                    "E03-T06: pos-evento expirado deve esperar keyframe para fechar.");
            scenario.read(3, false, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::recording &&
                        scenario.event_log.str().find("EVENT_RESUMED frame=3\n") != std::string::npos,
                    "E03-T06: retomada deve cancelar o fechamento.");
            scenario.finish();
            require(scenario.packet_sink.getRecordings().size() == 1, "E03-T06: deve manter uma gravacao.");
            requireRange(scenario.packet_sink.getRecordings()[0], 0, 3);
        }
        std::cout << "PASS: E03-T06 retomada durante fechamento conserva a gravacao.\n";

        {
            Scenario scenario({1, 100, 0});
            scenario.read(0, true, true);
            scenario.read(1, false, false, true);
            scenario.read(2, false);
            scenario.read(3, true);
            require(scenario.event_recorder.getRecordingState() == RecordingState::idle &&
                        scenario.event_recorder.getCompletedRecordingCount() == 1,
                    "E03-T07: chegada de keyframe deve concluir a gravacao.");
            requireRange(scenario.packet_sink.getRecordings().at(0), 0, 2);
            const auto remaining = scenario.encoded_packet_queue.takeAllPackets();
            require(remaining.size() == 1 && remaining[0]->pts == 3 &&
                        (remaining[0]->flags & AV_PKT_FLAG_KEY) != 0,
                    "E03-T07: keyframe do corte deve permanecer na frente da fila.");
        }
        std::cout << "PASS: E03-T07 fechamento preserva o keyframe na fila.\n";

        {
            for (const RecordingState state : {RecordingState::recording, RecordingState::post_event,
                                               RecordingState::closing_at_keyframe}) {
                Scenario scenario({1, 100, state == RecordingState::closing_at_keyframe ? 0U : 2U});
                scenario.read(0, true, true);
                std::int64_t next_id = 1;
                if (state != RecordingState::recording) {
                    scenario.read(1, false, false, true);
                    next_id = 2;
                }
                require(scenario.event_recorder.getRecordingState() == state, "E03-T08: estado inicial incorreto.");
                scenario.encoded_packet_queue.pushPacket(makePacket(next_id, false));
                scenario.encoded_packet_queue.pushPacket(makePacket(next_id + 1, true));
                require(scenario.encoded_packet_queue.getCountedPacketCount() == 0 &&
                            scenario.encoded_packet_queue.getQueuedPacketCount() == 2,
                        "E03-T08: os restantes devem estar a frente da fronteira.");
                scenario.finish();
                require(scenario.event_recorder.getCompletedRecordingCount() == 1 &&
                            scenario.event_log.str().find("SHUTDOWN_FLUSHED packets=2\n") != std::string::npos,
                        "E03-T08: encerramento ativo deve gravar os restantes.");
                requireRange(scenario.packet_sink.getRecordings().at(0), 0, next_id + 1);
            }
            Scenario waiting({1, 100, 0});
            waiting.read(0, false, true);
            waiting.encoded_packet_queue.pushPacket(makePacket(1, false));
            waiting.encoded_packet_queue.pushPacket(makePacket(2, true));
            waiting.encoded_packet_queue.pushPacket(makePacket(3, false));
            waiting.finish();
            requireRange(waiting.packet_sink.getRecordings().at(0), 2, 3);
            Scenario without_keyframe({1, 100, 0});
            without_keyframe.read(0, false, true);
            without_keyframe.encoded_packet_queue.pushPacket(makePacket(1, false));
            without_keyframe.finish();
            require(without_keyframe.packet_sink.getRecordings().empty() &&
                        without_keyframe.event_log.str().find("EVENT_DISCARDED_NO_KEYFRAME\n") != std::string::npos,
                    "E03-T08: espera sem keyframe deve descartar.");
            Scenario idle({1, 100, 0});
            idle.encoded_packet_queue.pushPacket(makePacket(0, true));
            idle.finish();
            require(idle.packet_sink.getRecordings().empty() &&
                        idle.event_log.str() == "SHUTDOWN_DISCARDED_IDLE_PACKETS packets=1\n",
                    "E03-T08: idle deve descartar os restantes.");
        }
        std::cout << "PASS: E03-T08 encerramento nos cinco estados preserva ou descarta os restantes conforme o estado.\n";

        {
            const auto recordings = simulateBaseline(0, true);
            require(!recordings.empty(), "E03-T09: simulacao deve produzir gravacoes.");
            std::int64_t previous_last = -1;
            for (const auto& recording : recordings) {
                require(!recording.packet_ids.empty() && recording.packet_ids.front() > previous_last,
                        "E03-T09: gravacoes nao devem se sobrepor.");
                requireRange(recording, recording.packet_ids.front(), recording.packet_ids.back());
                previous_last = recording.packet_ids.back();
            }
            for (const auto& interval : std::array<std::pair<std::int64_t, std::int64_t>, 5>{
                     {{118, 205}, {404, 464}, {875, 921}, {929, 972}, {1014, 1100}}}) {
                for (std::int64_t packet_id = interval.first; packet_id <= interval.second; ++packet_id) {
                    require(std::any_of(recordings.begin(), recordings.end(), [packet_id](const FakeRecording& recording) {
                        return std::find(recording.packet_ids.begin(), recording.packet_ids.end(), packet_id) !=
                               recording.packet_ids.end();
                    }), "E03-T09: todo pacote de movimento deve estar em alguma gravacao.");
                }
            }
        }
        std::cout << "PASS: E03-T09 rajadas e fronteira ate 32 pacotes a frente preservam as invariantes.\n";

        {
            Scenario scenario({4, 100, 1});
            scenario.read(0, true);
            scenario.read(1, false, true);
            scenario.read(2, false, false, true);
            scenario.read(3, false);
            scenario.read(4, true);
            scenario.finish();
            const std::string expected_log =
                "EVENT_STARTED frame=1\n"
                "MP4_STARTED path=\"recording-1.mp4\"\n"
                "PRE_EVENT_WRITTEN packets=2\n"
                "EVENT_ENDED frame=2\n"
                "POST_EVENT_EXPIRED frame=3\n"
                "MP4_SAVED path=\"recording-1.mp4\" decoded_reads=4 encoded_packets=4\n"
                "SHUTDOWN_DISCARDED_IDLE_PACKETS packets=1\n";
            require(scenario.event_log.str() == expected_log, "E03-T10: texto ou ordem dos logs mudou.");
        }
        std::cout << "PASS: E03-T10 texto e ordem exatos dos logs de pre-evento, pos-evento e fechamento.\n";

        {
            EncodedPacketQueue queue;
            FakeEventPacketSink sink;
            std::ostringstream log;
            for (const EventRecorderConfig config : {EventRecorderConfig{1, 0, 0}, EventRecorderConfig{5, 4, 0}}) {
                bool rejected = false;
                try {
                    EventRecorder recorder(queue, sink, config, log);
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                require(rejected, "E03-T11: teto zero ou menor que lookback deve ser rejeitado.");
            }
        }
        std::cout << "PASS: E03-T11 configuracoes invalidas rejeitadas.\n";

        {
            Scenario scenario({333, 1532, 300});
            appendCountedPackets(scenario, 0, 120, 0, PacketTimestampIssue::missing_timestamp);
            scenario.event_recorder.processDecodedFrame(motionEdges(true, false), 120);
            require(scenario.event_recorder.getRecordingState() == RecordingState::recording,
                    "E06-T02: deve retomar no keyframe valido do mesmo lote.");
            requireTimestampLog(scenario, "missing_timestamp", 120, 60, true);
            scenario.finish();
            require(scenario.packet_sink.getRecordings().size() == 1, "E06-T02: primeiro keyframe invalido nao pode abrir arquivo.");
            requireRange(scenario.packet_sink.getRecordings()[0], 60, 120);
        }
        std::cout << "PASS: E06-T02 primeiro keyframe sem carimbo descartado; retoma em K60 e descarta 60 pacotes.\n";

        {
            Scenario scenario({333, 1532, 300});
            scenario.read(0, true, true);
            appendCountedPackets(scenario, 1, 120, 10, PacketTimestampIssue::non_increasing_dts);
            scenario.event_recorder.processDecodedFrame(motionEdges(false, false), 120);
            requireTimestampLog(scenario, "non_increasing_dts", 120, 50, true);
            scenario.finish();
            require(scenario.packet_sink.getRecordings().size() == 2, "E06-T03: deve separar duas gravacoes.");
            requireRange(scenario.packet_sink.getRecordings()[0], 0, 9);
            requireRange(scenario.packet_sink.getRecordings()[1], 60, 120);
        }
        std::cout << "PASS: E06-T03 DTS repetido fecha em 9, descarta 50 pacotes e retoma em 60 no mesmo lote.\n";

        {
            Scenario scenario({333, 1532, 300});
            scenario.read(0, true, true);
            appendCountedPackets(scenario, 1, 5, 3, PacketTimestampIssue::non_increasing_dts);
            scenario.event_recorder.processDecodedFrame(motionEdges(false, false), 5);
            require(scenario.event_recorder.getRecordingState() == RecordingState::waiting_keyframe,
                    "E06-T04: deve esperar keyframe depois de fechar.");
            requireRange(scenario.packet_sink.getRecordings().at(0), 0, 2);
            requireTimestampLog(scenario, "non_increasing_dts", 5, 3, false);
            for (std::int64_t packet_id = 6; packet_id < 60; ++packet_id) {
                scenario.read(packet_id, false);
                require(scenario.packet_sink.getRecordings().size() == 1, "E06-T04: nao pode abrir antes do keyframe.");
            }
            scenario.read(60, true);
            scenario.finish();
            require(scenario.packet_sink.getRecordings().size() == 2, "E06-T04: keyframe seguinte deve abrir novo arquivo.");
            requireRange(scenario.packet_sink.getRecordings()[1], 60, 60);
        }
        std::cout << "PASS: E06-T04 sem keyframe no lote, fecha o prefixo e espera K60 em leituras seguintes.\n";

        {
            Scenario scenario({1, 100, 100});
            scenario.read(0, true, true);
            scenario.read(1, false, false, true);
            appendCountedPackets(scenario, 2, 5, 3, PacketTimestampIssue::non_increasing_dts, 4);
            scenario.event_recorder.processDecodedFrame(motionEdges(false, false), 5);
            require(scenario.event_recorder.getRecordingState() == RecordingState::idle &&
                        !scenario.packet_sink.checkIfRecordingIsOpen(), "E06-T05: pos-evento invalido deve fechar e voltar a idle.");
            require(scenario.packet_sink.getRecordings().size() == 1, "E06-T05: nao deve retomar no keyframe do lote.");
            requireRange(scenario.packet_sink.getRecordings()[0], 0, 2);
            requireTimestampLog(scenario, "non_increasing_dts", 5, 3, false);
            scenario.finish();
        }
        std::cout << "PASS: E06-T05 invalido no pos-evento fecha e descarta a continuacao, sem retomada.\n";

        {
            for (const bool keyframe_in_batch : {false, true}) {
                Scenario scenario({1, 100, 0});
                scenario.read(0, true, true);
                scenario.read(1, false, false, true);
                appendCountedPackets(scenario, 2, 5, 3, PacketTimestampIssue::non_increasing_dts,
                                     keyframe_in_batch ? 4 : -1);
                scenario.event_recorder.processDecodedFrame(motionEdges(false, false), 5);
                require(scenario.event_recorder.getRecordingState() == RecordingState::idle &&
                            !scenario.packet_sink.checkIfRecordingIsOpen(), "E06-T06: fechamento invalido deve voltar a idle.");
                requireRange(scenario.packet_sink.getRecordings().at(0), 0, 2);
                requireTimestampLog(scenario, "non_increasing_dts", 5, keyframe_in_batch ? 1 : 3, false);
                require(scenario.encoded_packet_queue.getCountedPacketCount() == (keyframe_in_batch ? 2 : 0),
                        "E06-T06: keyframe que delimitou a retirada deve permanecer na fila.");
                scenario.finish();
            }
        }
        std::cout << "PASS: E06-T06 invalido durante fechamento fecha sem retomar nem finalizar duas vezes.\n";

        {
            Scenario scenario({333, 1532, 300});
            constexpr std::array<std::uint64_t, 5> starts{118, 404, 875, 929, 1014};
            constexpr std::array<std::uint64_t, 5> ends{205, 464, 921, 972, 1100};
            for (std::uint64_t frame_index = 0; frame_index < 1181; ++frame_index) {
                auto packet = makePacket(static_cast<std::int64_t>(frame_index), frame_index % 60 == 0);
                packet->pts = AV_NOPTS_VALUE;
                packet->dts = AV_NOPTS_VALUE;
                scenario.encoded_packet_queue.pushPacket(std::move(packet));
                scenario.encoded_packet_queue.advanceCountedPackets(1);
                scenario.event_recorder.processDecodedFrame(
                    motionEdges(std::find(starts.begin(), starts.end(), frame_index) != starts.end(),
                                std::find(ends.begin(), ends.end(), frame_index) != ends.end()), frame_index
                );
            }
            scenario.finish();
            require(scenario.packet_sink.getRecordings().empty() && scenario.event_recorder.getCompletedRecordingCount() == 0,
                    "E06-T07: fonte sem carimbos nao deve criar gravacoes.");
            require(scenario.event_log.str().find("EVENT_INVALID_TIMESTAMP reason=missing_timestamp ") != std::string::npos,
                    "E06-T07: descarte deve registrar carimbo ausente.");
        }
        std::cout << "PASS: E06-T07 baseline inteiro sem carimbos nao cria arquivos nem lanca excecao.\n";

        {
            Scenario scenario({333, 1532, 300});
            scenario.read(0, true, true);
            appendCountedPackets(scenario, 1, 5, 3, PacketTimestampIssue::pts_before_dts);
            scenario.event_recorder.processDecodedFrame(motionEdges(false, false), 5);
            require(scenario.event_recorder.getRecordingState() == RecordingState::waiting_keyframe,
                    "E06-T08: pts anterior ao dts deve fechar e esperar keyframe.");
            requireTimestampLog(scenario, "pts_before_dts", 5, 3, false);
            requireRange(scenario.packet_sink.getRecordings().at(0), 0, 2);
            scenario.read(60, true);
            scenario.finish();
            requireRange(scenario.packet_sink.getRecordings().at(1), 60, 60);
        }
        std::cout << "PASS: E06-T08 pts menor que dts fecha o prefixo e retoma somente em keyframe.\n";

        {
            Scenario scenario({333, 1532, 300});
            scenario.read(0, true, true);
            scenario.read(1, false);
            for (std::int64_t packet_id = 2; packet_id <= 6; ++packet_id) {
                auto packet = makePacket(packet_id, packet_id == 5);
                if (packet_id == 3) {
                    packet->dts = 2;
                }
                scenario.encoded_packet_queue.pushPacket(std::move(packet));
            }
            scenario.finish();
            require(scenario.packet_sink.getRecordings().size() == 2 &&
                        scenario.event_recorder.getCompletedRecordingCount() == 2,
                    "E06-T09: encerramento deve finalizar prefixo e retomada.");
            requireRange(scenario.packet_sink.getRecordings()[0], 0, 2);
            requireRange(scenario.packet_sink.getRecordings()[1], 5, 6);
            requireTimestampLog(scenario, "non_increasing_dts", 1, 2, true);
        }
        std::cout << "PASS: E06-T09 encerramento valida os restantes e finaliza os dois trechos validos.\n";
        std::cout << "PASS: E06-T10 fluxo valido preservado pelos testes E03-T01, E03-T02 e E03-T10 acima.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
