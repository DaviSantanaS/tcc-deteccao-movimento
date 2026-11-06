#!/usr/bin/env bash
set -euo pipefail

show_usage() {
  printf '%s\n' \
    'Uso: bash run_rtsp_recording.sh [arquivo_h264] [diretorio_saida] [pre_evento_s]' \
    '     bash run_rtsp_recording.sh --webcam [diretorio_saida] [pre_evento_s]' \
    'Padroes: ~/Vídeos/tcc/video_h264_fhd_60fps.mp4, recordings do projeto, 5 segundos de pre-evento.' \
    'Webcam: /dev/video0 em YUYV, 640x480 a 30 FPS; FFmpeg codifica H.264 para a entrada RTSP.' \
    'RTSP local: rtsp://127.0.0.1:8554/video; MP4s por evento com pre-evento.' \
    'SHOW_PREVIEW=0 desativa a janela; RTSP_TRANSPORT=tcp troca UDP por TCP no publicador e na previa.' \
    'MEDIAMTX_BIN e OPENCV_DIR permitem ajustar os caminhos das dependencias.' \
    'Ctrl+C finaliza o gravador antes de encerrar a transmissao. MP4s e logs sao preservados.'
}

fail() {
  printf '[fatal] %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "Comando nao encontrado: $1"
}

require_free_port() {
  local socket_options="$1" port="$2" listeners
  listeners="$(ss -H "${socket_options}" "sport = :${port}")" ||
    fail "Nao foi possivel verificar a porta ${port}."
  [[ -z "${listeners}" ]] || fail "Porta ${port} ocupada; nenhum servico existente sera encerrado."
}

stop_owned_process() {
  local process_id="$1" initial_signal="$2" grace_seconds="$3" deadline
  [[ -n "${process_id}" ]] || return 0
  if kill -0 "${process_id}" 2>/dev/null; then
    kill -s "${initial_signal}" -- "-${process_id}" 2>/dev/null || true
    deadline=$((SECONDS + grace_seconds))
    while kill -0 "${process_id}" 2>/dev/null && (( SECONDS < deadline )); do
      sleep 0.1
    done
    if kill -0 "${process_id}" 2>/dev/null; then
      printf '[aviso] Encerramento forcado do processo %s; a saida parcial pode estar incompleta.\n' "${process_id}" >&2
      kill -s KILL -- "-${process_id}" 2>/dev/null || true
    fi
  fi
  wait "${process_id}" 2>/dev/null || true
}

cleanup() {
  local exit_status=$?
  trap '' INT TERM
  set +e
  stop_owned_process "${recorder_pid}" INT 10
  stop_owned_process "${preview_pid}" TERM 3
  stop_owned_process "${publisher_pid}" TERM 3
  stop_owned_process "${mediamtx_pid}" TERM 3
  if [[ -n "${log_viewer_pid}" ]]; then
    kill "${log_viewer_pid}" 2>/dev/null || true
    wait "${log_viewer_pid}" 2>/dev/null || true
  fi
  if [[ -n "${run_directory}" ]]; then
    [[ ! -f "${run_directory}/recording.log" ]] || tail -n 5 "${run_directory}/recording.log"
    printf '\nGravacoes: %s\nLogs preservados: %s\n' "${output_directory}" "${run_directory}"
  fi
  return "${exit_status}"
}

wait_for_rtsp_server() {
  local deadline=$((SECONDS + 15)) listeners
  while (( SECONDS < deadline )); do
    kill -0 "${mediamtx_pid}" 2>/dev/null || fail "MediaMTX encerrou. Consulte ${run_directory}/mediamtx.log"
    listeners="$(ss -H -ltn 'sport = :8554')" || fail 'Nao foi possivel consultar o listener RTSP.'
    [[ -z "${listeners}" ]] || return 0
    sleep 0.2
  done
  fail "MediaMTX nao abriu o RTSP em 15 segundos. Consulte ${run_directory}/mediamtx.log"
}

wait_for_h264_publication() {
  local deadline=$((SECONDS + 15)) codec_name remaining_seconds probe_timeout
  while (( SECONDS < deadline )); do
    kill -0 "${mediamtx_pid}" 2>/dev/null || fail 'MediaMTX encerrou durante a publicacao.'
    kill -0 "${publisher_pid}" 2>/dev/null || fail "FFmpeg encerrou. Consulte ${run_directory}/ffmpeg.log"
    remaining_seconds=$((deadline - SECONDS))
    probe_timeout=3
    (( remaining_seconds >= probe_timeout )) || probe_timeout="${remaining_seconds}"
    if codec_name="$(timeout -k 1 "${probe_timeout}" ffprobe -v error \
        -rtsp_transport "${rtsp_transport}" -select_streams v:0 \
        -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 \
        "${rtsp_url}" 2>>"${run_directory}/readiness.log")" && [[ "${codec_name}" == h264 ]]; then
      return 0
    fi
    sleep 0.2
  done
  fail "Publicacao H.264 nao ficou pronta em 15 segundos. Consulte ${run_directory}/ffmpeg.log"
}

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  show_usage
  exit 0
fi
if (( $# > 3 )); then
  show_usage >&2
  exit 1
fi

repo_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
input_source="${1-${HOME}/Vídeos/tcc/video_h264_fhd_60fps.mp4}"
source_is_webcam=0
if [[ "${input_source}" == --webcam ]]; then
  input_source='/dev/video0'
  source_is_webcam=1
fi
output_directory="${2-${repo_directory}/recordings}"
pre_event_seconds="${3-5}"
rtsp_transport="${RTSP_TRANSPORT:-udp}"
show_preview="${SHOW_PREVIEW:-1}"
mediamtx_binary="${MEDIAMTX_BIN:-/home/davi/Downloads/tcc/mediamtx}"
opencv_directory="${OPENCV_DIR:-/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4}"
build_directory="${repo_directory}/motion_mog2/cmake-build-debug"
rtsp_url='rtsp://127.0.0.1:8554/video'

[[ "${rtsp_transport}" == udp || "${rtsp_transport}" == tcp ]] || fail 'RTSP_TRANSPORT deve ser udp ou tcp.'
[[ "${show_preview}" == 0 || "${show_preview}" == 1 ]] || fail 'SHOW_PREVIEW deve ser 0 ou 1.'
[[ -n "${output_directory}" ]] || fail 'O diretorio de saida nao pode ser vazio.'
if [[ "${source_is_webcam}" == 1 ]]; then
  [[ -c "${input_source}" ]] || fail "Dispositivo de webcam inexistente: ${input_source}"
  [[ -r "${input_source}" && -w "${input_source}" ]] || fail "Webcam sem permissao de leitura/escrita: ${input_source}"
else
  [[ -f "${input_source}" && -r "${input_source}" ]] || fail "Video inexistente ou sem leitura: ${input_source}"
fi
[[ -x "${mediamtx_binary}" ]] || fail "MediaMTX inexistente ou sem permissao de execucao: ${mediamtx_binary}"

for required_command in ffmpeg ffprobe cmake ss setsid timeout realpath mktemp tail; do
  require_command "${required_command}"
done
input_source="$(realpath -- "${input_source}")"
output_directory="$(realpath -m -- "${output_directory}")"
if [[ "${source_is_webcam}" == 0 ]]; then
  source_codec="$(timeout -k 1 5 ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 \
    "${input_source}")" || fail 'Nao foi possivel consultar o video de entrada.'
  [[ "${source_codec}" == h264 ]] || fail "O MVP exige H.264; codec encontrado: ${source_codec:-nenhum}. Nao sera feita conversao."
fi

require_free_port -ltn 8554
require_free_port -lun 8000
require_free_port -lun 8001
if [[ "${show_preview}" == 1 ]] &&
   { ! command -v ffplay >/dev/null 2>&1 || [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; }; then
  printf '[aviso] FFplay ou ambiente grafico indisponivel; seguindo sem janela.\n' >&2
  show_preview=0
fi

mediamtx_pid=''
publisher_pid=''
preview_pid=''
recorder_pid=''
log_viewer_pid=''
run_directory=''
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p -- "${output_directory}"
run_directory="$(mktemp -d "${output_directory}/.rtsp-run.XXXXXX")"
printf '%s\n' \
  'logLevel: info' \
  'logDestinations: [stdout]' \
  'api: false' 'metrics: false' 'pprof: false' 'playback: false' \
  'rtsp: true' 'rtspAddress: 127.0.0.1:8554' \
  'rtspTransports: [udp, tcp]' \
  'rtpAddress: 127.0.0.1:8000' 'rtcpAddress: 127.0.0.1:8001' \
  'rtmp: false' 'hls: false' 'webrtc: false' 'srt: false' 'moq: false' \
  'paths:' '  video:' '    source: publisher' '    overridePublisher: false' \
  >"${run_directory}/mediamtx.yml"
printf 'Fonte: %s\nRTSP: %s\nTransporte do publicador/previa: %s\nLogs: %s\n' \
  "${input_source}" "${rtsp_url}" "${rtsp_transport}" "${run_directory}"

printf '\n[1/4] Compilando o gravador...\n'
cmake -S "${repo_directory}/motion_mog2" -B "${build_directory}" \
  -DOpenCV_DIR="${opencv_directory}" >"${run_directory}/build.log" 2>&1 ||
  fail "Configuracao CMake falhou. Consulte ${run_directory}/build.log"
cmake --build "${build_directory}" --target motion_mog2 --parallel 2 \
  >>"${run_directory}/build.log" 2>&1 || fail "Build falhou. Consulte ${run_directory}/build.log"

# setsid protege os filhos do Ctrl+C do terminal ate o gravador finalizar o MP4.
printf '\n[2/4] Subindo MediaMTX...\n'
setsid "${mediamtx_binary}" "${run_directory}/mediamtx.yml" >"${run_directory}/mediamtx.log" 2>&1 &
mediamtx_pid=$!
wait_for_rtsp_server

if [[ "${source_is_webcam}" == 1 ]]; then
  printf '\n[3/4] Capturando webcam e codificando H.264 para a entrada RTSP...\n'
  publisher_input_arguments=(-f v4l2 -input_format yuyv422 -video_size 640x480 -framerate 30 -i "${input_source}")
  publisher_video_arguments=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p
    -g 60 -keyint_min 60 -sc_threshold 0 -bf 0)
else
  printf '\n[3/4] Publicando H.264 em loop, sem transcoding...\n'
  publisher_input_arguments=(-re -stream_loop -1 -i "${input_source}")
  publisher_video_arguments=(-c:v copy)
fi
setsid ffmpeg -hide_banner -loglevel info -nostdin \
  "${publisher_input_arguments[@]}" -map 0:v:0 -an "${publisher_video_arguments[@]}" \
  -f rtsp -rtsp_transport "${rtsp_transport}" "${rtsp_url}" \
  >"${run_directory}/ffmpeg.log" 2>&1 &
publisher_pid=$!
wait_for_h264_publication

if [[ "${show_preview}" == 1 ]]; then
  setsid ffplay -hide_banner -loglevel warning -vcodec h264_cuvid \
    -rtsp_transport "${rtsp_transport}" -fflags nobuffer -flags low_delay -framedrop \
    -window_title 'TCC - RTSP e gravacao MP4' "${rtsp_url}" \
    >"${run_directory}/ffplay.log" 2>&1 &
  preview_pid=$!
fi

printf '\n[4/4] Gravando MP4s por evento; Ctrl+C finaliza o evento atual...\n'
setsid "${build_directory}/motion_mog2" "${rtsp_url}" "${output_directory}" "${pre_event_seconds}" 1.0 2 3 5 "${rtsp_transport}" \
  >"${run_directory}/recording.log" 2>&1 &
recorder_pid=$!
tail -n +1 -s 0.2 -f "${run_directory}/recording.log" &
log_viewer_pid=$!

while kill -0 "${recorder_pid}" 2>/dev/null; do
  kill -0 "${mediamtx_pid}" 2>/dev/null || fail "MediaMTX encerrou. Consulte ${run_directory}/mediamtx.log"
  kill -0 "${publisher_pid}" 2>/dev/null || fail "FFmpeg encerrou. Consulte ${run_directory}/ffmpeg.log"
  if [[ -n "${preview_pid}" ]] && ! kill -0 "${preview_pid}" 2>/dev/null; then
    wait "${preview_pid}" 2>/dev/null || true
    preview_pid=''
    printf '[aviso] Previa encerrada; a gravacao continua. Consulte %s/ffplay.log\n' "${run_directory}" >&2
  fi
  sleep 0.2
done

recorder_exit_status=0
wait "${recorder_pid}" || recorder_exit_status=$?
recorder_pid=''
kill -0 "${mediamtx_pid}" 2>/dev/null || fail "MediaMTX encerrou. Consulte ${run_directory}/mediamtx.log"
kill -0 "${publisher_pid}" 2>/dev/null || fail "FFmpeg encerrou. Consulte ${run_directory}/ffmpeg.log"
exit "${recorder_exit_status}"
