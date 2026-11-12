#include "detectors/MotionHysteresis.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            require(motion_hysteresis.getWarmupFrameCount() == 60,
                    "E01-T01: aquecimento configurado incorreto.");
            for (std::uint64_t frame_index = 0; frame_index < 60; ++frame_index) {
                const MotionState motion_state = motion_hysteresis.update(1.0, frame_index);
                require(!motion_state.detected && !motion_state.started && !motion_state.ended &&
                            !motion_state.active && motion_state.foreground_ratio == 0.0 &&
                            !motion_hysteresis.checkIfMotionIsActive(),
                        "E01-T01: aquecimento deve ignorar movimento.");
            }
        }
        std::cout << "PASS: E01-T01 aquecimento nao inicia movimento.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            const MotionState first_frame = motion_hysteresis.update(0.5, 60);
            require(first_frame.detected && !first_frame.started && !first_frame.active &&
                        !first_frame.ended && first_frame.foreground_ratio == 0.5,
                    "E01-T02: primeiro quadro nao deve iniciar movimento.");
            const MotionState second_frame = motion_hysteresis.update(0.5, 61);
            require(second_frame.detected && second_frame.started && second_frame.active &&
                        !second_frame.ended && motion_hysteresis.checkIfMotionIsActive(),
                    "E01-T02: segundo quadro deve iniciar movimento.");
            const MotionState third_frame = motion_hysteresis.update(0.5, 62);
            require(third_frame.active && !third_frame.started && !third_frame.ended,
                    "E01-T02: inicio nao deve ser repetido durante movimento ativo.");
        }
        std::cout << "PASS: E01-T02 inicio exige N quadros seguidos.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            require(!motion_hysteresis.update(0.5, 60).started,
                    "E01-T03: primeiro quadro nao deve iniciar.");
            const MotionState idle_frame = motion_hysteresis.update(0.0, 61);
            require(!idle_frame.detected && !idle_frame.started && !idle_frame.active,
                    "E01-T03: quadro sem movimento deve interromper a sequencia.");
            require(!motion_hysteresis.update(0.5, 62).started &&
                        !motion_hysteresis.checkIfMotionIsActive(),
                    "E01-T03: quadro isolado anterior nao deve ser contado.");
            require(motion_hysteresis.update(0.5, 63).started &&
                        motion_hysteresis.checkIfMotionIsActive(),
                    "E01-T03: dois novos quadros seguidos devem iniciar.");
        }
        std::cout << "PASS: E01-T03 interrupcao zera a contagem de inicio.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            motion_hysteresis.update(0.5, 60);
            require(motion_hysteresis.update(0.5, 61).started,
                    "E01-T04: preparo deve ativar movimento.");
            for (std::uint64_t frame_index = 62; frame_index <= 64; ++frame_index) {
                const MotionState motion_state = motion_hysteresis.update(0.0, frame_index);
                const bool last_frame = frame_index == 64;
                require(!motion_state.detected && !motion_state.started &&
                            motion_state.ended == last_frame && motion_state.active == !last_frame &&
                            motion_hysteresis.checkIfMotionIsActive() == !last_frame,
                        "E01-T04: fim deve ocorrer somente no terceiro quadro sem movimento.");
            }
            const MotionState idle_frame = motion_hysteresis.update(0.0, 65);
            require(!idle_frame.active && !idle_frame.started && !idle_frame.ended,
                    "E01-T04: fim nao deve ser repetido durante inatividade.");
        }
        std::cout << "PASS: E01-T04 fim exige M quadros seguidos.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            motion_hysteresis.update(0.5, 60);
            require(motion_hysteresis.update(0.5, 61).started,
                    "E01-T05: preparo deve ativar movimento.");
            for (std::uint64_t frame_index = 62; frame_index <= 67; ++frame_index) {
                const double foreground_ratio = frame_index == 64 ? 0.5 : 0.0;
                const MotionState motion_state = motion_hysteresis.update(foreground_ratio, frame_index);
                const bool last_frame = frame_index == 67;
                require(!motion_state.started && motion_state.ended == last_frame &&
                            motion_state.active == !last_frame,
                        "E01-T05: movimento intermediario deve zerar a contagem de fim.");
            }
        }
        std::cout << "PASS: E01-T05 movimento intermediario zera a contagem de fim.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            const MotionState equal_frame = motion_hysteresis.update(0.01, 60);
            require(equal_frame.detected && equal_frame.foreground_ratio == 0.01,
                    "E01-T06: razao igual ao limiar deve ser detectada e preservada.");
            require(motion_hysteresis.update(0.01, 61).started,
                    "E01-T06: limiar inclusivo deve alimentar a contagem de inicio.");
            const MotionState below_frame = motion_hysteresis.update(0.009, 62);
            require(!below_frame.detected && below_frame.foreground_ratio == 0.009,
                    "E01-T06: razao abaixo do limiar nao deve ser detectada.");
        }
        std::cout << "PASS: E01-T06 limiar inclusivo.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 2, 3, 60);
            std::mt19937 random_engine(12345);
            int started_count = 0;
            int ended_count = 0;
            for (std::uint64_t frame_index = 0; frame_index < 10000; ++frame_index) {
                const double foreground_ratio = random_engine() % 2 == 0 ? 0.5 : 0.0;
                const MotionState motion_state = motion_hysteresis.update(foreground_ratio, frame_index);
                require(!(motion_state.started && motion_state.ended),
                        "E01-T07: inicio e fim nao podem ocorrer no mesmo quadro.");
                require(motion_state.active == motion_hysteresis.checkIfMotionIsActive(),
                        "E01-T07: estado devolvido deve coincidir com a consulta.");
                started_count += motion_state.started;
                ended_count += motion_state.ended;
            }
            require(started_count > 0 && ended_count > 0,
                    "E01-T07: sequencia deve exercitar inicios e fins.");
        }
        std::cout << "PASS: E01-T07 inicio e fim exclusivos em 10000 quadros.\n";

        {
            MotionHysteresis motion_hysteresis(0.01, 1, 1, 0);
            require(motion_hysteresis.getWarmupFrameCount() == 0,
                    "E01-T08: aquecimento deve ser zero.");
            const MotionState first_frame = motion_hysteresis.update(0.5, 0);
            require(first_frame.detected && first_frame.started && first_frame.active && !first_frame.ended,
                    "E01-T08: N=1 deve iniciar no primeiro quadro.");
            const MotionState second_frame = motion_hysteresis.update(0.0, 1);
            require(!second_frame.detected && !second_frame.started && !second_frame.active &&
                        second_frame.ended && !motion_hysteresis.checkIfMotionIsActive(),
                    "E01-T08: M=1 deve terminar no primeiro quadro sem movimento.");
        }
        std::cout << "PASS: E01-T08 N=1 e M=1.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
