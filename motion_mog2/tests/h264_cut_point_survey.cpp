#include "ffmpeg/AvPacket.hpp"
#include "ffmpeg/H264NalUnits.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct InputDeadline {
    std::chrono::steady_clock::time_point started_at;
    double duration_seconds;

    bool checkIfExpired() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - this->started_at).count() >=
            this->duration_seconds;
    }
};

int interruptInputRead(void* context) {
    return static_cast<const InputDeadline*>(context)->checkIfExpired() ? 1 : 0;
}

struct InputContextDeleter {
    void operator()(AVFormatContext* context) const noexcept {
        avformat_close_input(&context);
    }
};

std::string getFfmpegErrorMessage(int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(status, buffer, sizeof(buffer));
    return buffer;
}

std::string redactSourceCredentials(const std::string& source) {
    if (source.rfind("rtsp://", 0) != 0) {
        return source;
    }
    const std::size_t authority_end = source.find_first_of("/?#", 7);
    const std::size_t credentials_end = source.rfind('@', authority_end);
    return credentials_end != std::string::npos && credentials_end >= 7
        ? "rtsp://" + source.substr(credentials_end + 1) : source;
}

void printUsage() {
    std::cout << "Uso: h264_cut_point_survey <fonte> [rtsp_transporte=tcp] [segundos=60]\n"
                 "fonte: arquivo H.264 ou URL rtsp://; credenciais sao omitidas na saida da ferramenta.\n"
                 "rtsp_transporte: tcp, udp ou auto; aplicado somente a RTSP.\n"
                 "segundos: prazo positivo de abertura e leitura. --help mostra esta ajuda.\n"
                 "source: fonte sem usuario e senha; seconds: tempo real desde a abertura.\n"
                 "packets: pacotes de video; key_packets: pacotes com AV_PKT_FLAG_KEY.\n"
                 "key_with_idr: keyframes com NAL 5; key_without_idr: keyframes sem NAL 5.\n"
                 "idr_without_key: pacotes com NAL 5 sem flag KEY; packets_with_sps: pacotes com NAL 7.\n"
                 "max_key_interval_packets: maior distancia entre indices de keyframes consecutivos.\n"
                 "max_idr_interval_packets: maior distancia entre indices de pacotes IDR consecutivos.\n"
                 "KEY_WITHOUT_IDR: indice de video iniciado em zero e tamanho, nos primeiros 20 casos.\n"
                 "Intervalos ficam em zero quando ha menos de duas ocorrencias.\n";
}

}

int main(int argument_count, char** argument_values) {
    if (argument_count == 1 || (argument_count == 2 && std::string(argument_values[1]) == "--help")) {
        printUsage();
        return argument_count == 1 ? 1 : 0;
    }
    if (argument_count > 4) {
        printUsage();
        return 1;
    }
    const std::string input_source = argument_values[1];
    const std::string safe_source = redactSourceCredentials(input_source);
    try {
        const std::string rtsp_transport = argument_count >= 3 ? argument_values[2] : "tcp";
        if (rtsp_transport != "tcp" && rtsp_transport != "udp" && rtsp_transport != "auto") {
            throw std::invalid_argument("Transporte deve ser tcp, udp ou auto.");
        }
        double duration_seconds = 60.0;
        if (argument_count == 4) {
            const std::string duration_argument = argument_values[3];
            std::size_t parsed_size = 0;
            duration_seconds = std::stod(duration_argument, &parsed_size);
            if (parsed_size != duration_argument.size()) {
                throw std::invalid_argument("Prazo deve ser um numero positivo e finito.");
            }
        }
        if (!std::isfinite(duration_seconds) || duration_seconds <= 0.0) {
            throw std::invalid_argument("Prazo deve ser um numero positivo e finito.");
        }

        // O prazo continua vivo ate fechar a entrada e tambem limita a abertura.
        InputDeadline deadline{std::chrono::steady_clock::now(), duration_seconds};
        AVFormatContext* opened_context = avformat_alloc_context();
        if (opened_context == nullptr) {
            throw std::runtime_error("Nao foi possivel alocar a entrada FFmpeg.");
        }
        opened_context->interrupt_callback.callback = interruptInputRead;
        opened_context->interrupt_callback.opaque = &deadline;
        AVDictionary* options = nullptr;
        const bool use_transport_option = input_source.rfind("rtsp://", 0) == 0 && rtsp_transport != "auto";
        if (use_transport_option) {
            const int status = av_dict_set(&options, "rtsp_transport", rtsp_transport.c_str(), 0);
            if (status < 0) {
                av_dict_free(&options);
                avformat_free_context(opened_context);
                throw std::runtime_error("Nao foi possivel configurar RTSP: " + getFfmpegErrorMessage(status));
            }
        }
        const int open_status = avformat_open_input(&opened_context, input_source.c_str(), nullptr, &options);
        av_dict_free(&options);
        const std::unique_ptr<AVFormatContext, InputContextDeleter> input_context{opened_context};
        if (open_status < 0) {
            throw std::runtime_error("Nao foi possivel abrir a entrada: " + getFfmpegErrorMessage(open_status));
        }
        const int information_status = avformat_find_stream_info(input_context.get(), nullptr);
        if (information_status < 0) {
            throw std::runtime_error("Nao foi possivel obter os streams: " + getFfmpegErrorMessage(information_status));
        }
        const int video_index = av_find_best_stream(input_context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_index < 0) {
            throw std::runtime_error("Stream de video ausente: " + getFfmpegErrorMessage(video_index));
        }
        const AVCodecParameters& parameters = *input_context->streams[video_index]->codecpar;
        if (parameters.codec_id != AV_CODEC_ID_H264) {
            throw std::runtime_error("A ferramenta exige video H.264.");
        }

        std::uint64_t packet_count = 0;
        std::uint64_t key_packet_count = 0;
        std::uint64_t key_with_idr_count = 0;
        std::uint64_t key_without_idr_count = 0;
        std::uint64_t idr_without_key_count = 0;
        std::uint64_t packets_with_sps_count = 0;
        std::uint64_t maximum_key_interval = 0;
        std::uint64_t maximum_idr_interval = 0;
        std::optional<std::uint64_t> previous_key_index;
        std::optional<std::uint64_t> previous_idr_index;
        std::vector<std::pair<std::uint64_t, int>> non_idr_key_packets;
        auto packet = allocateAvPacket();
        while (!deadline.checkIfExpired()) {
            av_packet_unref(packet.get());
            const int read_status = av_read_frame(input_context.get(), packet.get());
            if (read_status == AVERROR(EAGAIN)) {
                continue;
            }
            if (read_status < 0 || deadline.checkIfExpired()) {
                break;
            }
            if (packet->stream_index != video_index || packet->data == nullptr || packet->size <= 0) {
                continue;
            }
            const bool keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            const bool contains_idr = checkIfH264PacketContainsNalUnitType(*packet, parameters, 5);
            if (checkIfH264PacketContainsNalUnitType(*packet, parameters, 7)) {
                ++packets_with_sps_count;
            }
            if (keyframe) {
                ++key_packet_count;
                if (previous_key_index) {
                    maximum_key_interval = std::max(maximum_key_interval, packet_count - *previous_key_index);
                }
                previous_key_index = packet_count;
                if (contains_idr) {
                    ++key_with_idr_count;
                } else {
                    ++key_without_idr_count;
                    if (non_idr_key_packets.size() < 20) {
                        non_idr_key_packets.emplace_back(packet_count, packet->size);
                    }
                }
            }
            if (contains_idr) {
                if (!keyframe) {
                    ++idr_without_key_count;
                }
                if (previous_idr_index) {
                    maximum_idr_interval = std::max(maximum_idr_interval, packet_count - *previous_idr_index);
                }
                previous_idr_index = packet_count;
            }
            ++packet_count;
        }
        const double elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - deadline.started_at
        ).count();
        std::cout << "CUT_POINT_SURVEY source=" << safe_source << " seconds=" << std::fixed << std::setprecision(3)
                  << elapsed_seconds << " packets=" << packet_count << " key_packets=" << key_packet_count
                  << " key_with_idr=" << key_with_idr_count << " key_without_idr=" << key_without_idr_count
                  << " idr_without_key=" << idr_without_key_count << " packets_with_sps=" << packets_with_sps_count
                  << " max_key_interval_packets=" << maximum_key_interval
                  << " max_idr_interval_packets=" << maximum_idr_interval << '\n';
        for (const auto& [index, size] : non_idr_key_packets) {
            std::cout << "KEY_WITHOUT_IDR index=" << index << " size=" << size << '\n';
        }
        return packet_count > 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "[error] source=" << safe_source << ' ' << error.what() << '\n';
        return 1;
    }
}
