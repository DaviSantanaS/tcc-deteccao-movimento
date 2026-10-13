#include "AvCodecParameters.hpp"
#include "AvOutputFormatContext.hpp"
#include "VideoStreamParameters.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/defs.h>
#include <libavutil/mem.h>
}

int main() {
    try {
        AvOutputFormatContextPtr output_context = allocateMp4OutputContext();
        if (!output_context || !output_context->oformat ||
            !output_context->oformat->name ||
            std::strcmp(output_context->oformat->name, "mp4") != 0 ||
            output_context->iformat != nullptr || output_context->pb != nullptr ||
            output_context->nb_streams != 0) {
            throw std::runtime_error("A alocacao deve configurar MP4 sem abrir arquivo nem criar streams.");
        }

        AvOutputFormatContextPtr independent_output_context = allocateMp4OutputContext();
        if (!independent_output_context ||
            independent_output_context.get() == output_context.get()) {
            throw std::runtime_error("Duas alocacoes devem possuir contextos independentes.");
        }
        AVFormatContext* allocated_output_context = output_context.get();
        AvOutputFormatContextPtr moved_output_context = std::move(output_context);
        if (output_context || moved_output_context.get() != allocated_output_context ||
            std::strcmp(moved_output_context->oformat->name, "mp4") != 0 ||
            moved_output_context->pb != nullptr || moved_output_context->nb_streams != 0) {
            throw std::runtime_error("O movimento deve transferir a posse do mesmo contexto MP4.");
        }

        moved_output_context.reset();
        if (moved_output_context || !independent_output_context->oformat ||
            !independent_output_context->oformat->name ||
            std::strcmp(independent_output_context->oformat->name, "mp4") != 0 ||
            independent_output_context->pb != nullptr ||
            independent_output_context->nb_streams != 0) {
            throw std::runtime_error("reset() deve esvaziar seu proprietario sem invalidar o contexto independente.");
        }

        {
            AvOutputFormatContextPtr scoped_output_context = allocateMp4OutputContext();
            if (!scoped_output_context) {
                throw std::runtime_error("O contexto de escopo deve ser valido antes da destruicao automatica.");
            }
        }
        AvOutputFormatContextPtr empty_output_context;
        empty_output_context.reset();
        AvOutputFormatContextDeleter{}(nullptr);

        std::cout << "PASS: contexto MP4 alocado sem arquivo aberto nem streams.\n";
        std::cout << "PASS: alocacoes independentes e transferencia de posse sem alterar o endereco.\n";
        std::cout << "PASS: reset, destruicao por escopo e proprietario vazio sem erros.\n";

    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
