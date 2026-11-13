#include "ffmpeg/H264NalUnits.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<unsigned char> makeLengthPrefixedNals(int prefix_size) {
    std::vector<unsigned char> result;
    for (const unsigned char header : {0x67, 0x65}) {
        for (int byte_index = prefix_size - 1; byte_index >= 0; --byte_index) {
            result.push_back(static_cast<unsigned char>(std::size_t{2} >> (byte_index * 8)));
        }
        result.push_back(header);
        result.push_back(0x80);
    }
    return result;
}

}

int main() {
    try {
        const std::array<unsigned char, 6> sps4{0, 0, 0, 1, 0x67, 0x80};
        require(checkIfAnnexBContainsNalUnitType(sps4.data(), sps4.size(), 7), "SPS com codigo de quatro bytes nao encontrado.");
        std::cout << "PASS: E08-T01 codigo de inicio de quatro bytes identifica SPS.\n";

        const std::array<unsigned char, 5> idr3{0, 0, 1, 0x65, 0x80};
        require(checkIfAnnexBContainsNalUnitType(idr3.data(), idr3.size(), 5), "IDR com codigo de tres bytes nao encontrado.");
        std::cout << "PASS: E08-T02 codigo de inicio de tres bytes identifica IDR.\n";

        const std::array<unsigned char, 16> multiple{0, 0, 1, 0x09, 0x80, 0, 0, 0, 1, 0x06, 0x80, 0, 0, 1, 0x65, 0x80};
        require(checkIfAnnexBContainsNalUnitType(multiple.data(), multiple.size(), 5) &&
                !checkIfAnnexBContainsNalUnitType(multiple.data(), multiple.size(), 7), "Busca entre varios NAL incorreta.");
        std::cout << "PASS: E08-T03 AUD, SEI e IDR sao percorridos sem inventar SPS.\n";

        const std::array<unsigned char, 4> sps_low_ref{0, 0, 1, 0x27};
        require(checkIfAnnexBContainsNalUnitType(sps_low_ref.data(), sps_low_ref.size(), 7) &&
                checkIfAnnexBContainsNalUnitType(sps4.data(), sps4.size(), 7), "Tipo deve usar somente cinco bits.");
        std::cout << "PASS: E08-T04 cabecalhos 0x67 e 0x27 identificam tipo 7.\n";

        const std::array<unsigned char, 3> start3{0, 0, 1};
        const std::array<unsigned char, 4> start4{0, 0, 0, 1};
        require(!checkIfAnnexBContainsNalUnitType(nullptr, 100, 7) &&
                !checkIfAnnexBContainsNalUnitType(sps4.data(), 0, 7) &&
                !checkIfAnnexBContainsNalUnitType(start3.data(), start3.size(), 7) &&
                !checkIfAnnexBContainsNalUnitType(start4.data(), start4.size(), 7), "Bordas Annex B devem retornar falso.");
        std::cout << "PASS: E08-T05 nulo, vazio e codigo sem cabecalho retornam falso.\n";

        const std::array<unsigned char, 4> avcc_signature{1, 0x64, 0, 0x1f};
        require(checkIfBufferStartsWithAnnexBStartCode(start3.data(), start3.size()) &&
                checkIfBufferStartsWithAnnexBStartCode(start4.data(), start4.size()) &&
                !checkIfBufferStartsWithAnnexBStartCode(avcc_signature.data(), avcc_signature.size()) &&
                !checkIfBufferStartsWithAnnexBStartCode(nullptr, 4), "Deteccao do inicio Annex B incorreta.");
        std::cout << "PASS: E08-T06 distingue codigos Annex B da assinatura avcC.\n";

        for (int prefix_size = 1; prefix_size <= 4; ++prefix_size) {
            const auto data = makeLengthPrefixedNals(prefix_size);
            require(checkIfLengthPrefixedContainsNalUnitType(data.data(), data.size(), prefix_size, 5) &&
                    checkIfLengthPrefixedContainsNalUnitType(data.data(), data.size(), prefix_size, 7) &&
                    !checkIfLengthPrefixedContainsNalUnitType(data.data(), data.size(), prefix_size, 8), "Busca AVCC incorreta.");
        }
        std::cout << "PASS: E10-T01 prefixos de um, dois, tres e quatro bytes identificam IDR e SPS.\n";

        const std::array<unsigned char, 2> oversized{5, 0x65};
        const std::array<unsigned char, 5> maximum_size{0xff, 0xff, 0xff, 0xff, 0x65};
        require(!checkIfLengthPrefixedContainsNalUnitType(oversized.data(), oversized.size(), 1, 5) &&
                !checkIfLengthPrefixedContainsNalUnitType(maximum_size.data(), maximum_size.size(), 4, 5) &&
                !checkIfLengthPrefixedContainsNalUnitType(oversized.data(), 1, 2, 5) &&
                !checkIfLengthPrefixedContainsNalUnitType(nullptr, 100, 1, 5) &&
                !checkIfLengthPrefixedContainsNalUnitType(oversized.data(), oversized.size(), 0, 5) &&
                !checkIfLengthPrefixedContainsNalUnitType(oversized.data(), oversized.size(), 5, 5), "Tamanho AVCC invalido deve retornar falso.");
        std::cout << "PASS: E10-T02 tamanhos alem do buffer e prefixos invalidos sao rejeitados.\n";

        AVPacket packet{};
        AVCodecParameters parameters{};
        std::array<unsigned char, 4> annex_b_packet{0, 0, 1, 0x65};
        std::array<unsigned char, 4> annex_b_extra{0, 0, 0, 1};
        packet.data = annex_b_packet.data();
        packet.size = static_cast<int>(annex_b_packet.size());
        require(checkIfH264PacketContainsNalUnitType(packet, parameters, 5), "Extradata vazia deve selecionar Annex B.");
        parameters.extradata = annex_b_extra.data();
        parameters.extradata_size = static_cast<int>(annex_b_extra.size());
        require(checkIfH264PacketContainsNalUnitType(packet, parameters, 5), "Extradata Annex B deve selecionar Annex B.");
        for (int prefix_size = 1; prefix_size <= 4; ++prefix_size) {
            auto data = makeLengthPrefixedNals(prefix_size);
            std::array<unsigned char, 7> avcc_extra{1, 0x64, 0, 0x1f, static_cast<unsigned char>(0xfc | (prefix_size - 1)), 0xe0, 0};
            packet.data = data.data();
            packet.size = static_cast<int>(data.size());
            parameters.extradata = avcc_extra.data();
            parameters.extradata_size = static_cast<int>(avcc_extra.size());
            require(checkIfH264PacketContainsNalUnitType(packet, parameters, 5) &&
                    checkIfH264PacketContainsNalUnitType(packet, parameters, 7), "Extradata avcC deve selecionar seu comprimento de prefixo.");
            parameters.extradata_size = 6;
            require(!checkIfH264PacketContainsNalUnitType(packet, parameters, 5), "Extradata avcC curta deve retornar falso.");
        }
        std::cout << "PASS: E10-T03 extradata seleciona Annex B ou AVCC e seu tamanho de prefixo.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
