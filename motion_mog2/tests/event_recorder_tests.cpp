#include "recording/EventRecorder.hpp"

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
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
