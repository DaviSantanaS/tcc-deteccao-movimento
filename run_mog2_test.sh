#!/usr/bin/env bash
set -euo pipefail

show_usage() {
  echo "Uso: bash run_mog2_test.sh [video-ou-pasta] [motion_threshold_percent] [motion_start_frames] [motion_end_frames]"
  echo "Pasta padrao: ${VIDEO_DIR:-${HOME}/Vídeos/tcc}"
  echo "Exemplo: bash run_mog2_test.sh \"\$HOME/Vídeos/tcc\" 1.0 2 3"
  echo "Padroes: limiar=1%, inicio=2 frames, fim=3 frames."
  echo "Saida do detector: .run_mog2/motion_mog2.log"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  show_usage
  exit 0
fi

if (( $# > 4 )); then
  show_usage >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VIDEO_DIR="${VIDEO_DIR:-${HOME}/Vídeos/tcc}"
VIDEO_PATH="${1:-${VIDEO_DIR}}"
MOTION_THRESHOLD_PERCENT="${2-1.0}"
MOTION_START_FRAMES="${3-2}"
MOTION_END_FRAMES="${4-3}"
RTSP_URL="${RTSP_URL:-rtsp://127.0.0.1:8554/video}"
OPENCV_DIR="${OPENCV_DIR:-/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4}"
MEDIAMTX_BIN="${MEDIAMTX_BIN:-/home/davi/Downloads/tcc/mediamtx}"
BUILD_DIR="${ROOT_DIR}/motion_mog2/build"
RUN_DIR="${ROOT_DIR}/.run_mog2"

MEDIAMTX_PID=""
FFMPEG_PID=""
FFPLAY_PID=""

cleanup() {
  set +e

  if [[ -n "${FFPLAY_PID}" ]] && kill -0 "${FFPLAY_PID}" 2>/dev/null; then
    kill "${FFPLAY_PID}" 2>/dev/null || true
    wait "${FFPLAY_PID}" 2>/dev/null || true
  fi

  if [[ -n "${FFMPEG_PID}" ]] && kill -0 "${FFMPEG_PID}" 2>/dev/null; then
    kill "${FFMPEG_PID}" 2>/dev/null || true
    wait "${FFMPEG_PID}" 2>/dev/null || true
  fi

  if [[ -n "${MEDIAMTX_PID}" ]] && kill -0 "${MEDIAMTX_PID}" 2>/dev/null; then
    kill "${MEDIAMTX_PID}" 2>/dev/null || true
    wait "${MEDIAMTX_PID}" 2>/dev/null || true
  fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[fatal] Comando nao encontrado: $1" >&2
    exit 1
  fi
}

select_video_from_directory() {
  if [[ ! -d "${VIDEO_PATH}" ]]; then
    return
  fi

  local -a video_candidates=()
  local candidate
  for candidate in "${VIDEO_PATH}"/*; do
    if [[ ! -f "${candidate}" ]]; then
      continue
    fi
    case "${candidate,,}" in
      *.mp4|*.mkv|*.avi|*.mov|*.m4v|*.ts|*.webm)
        video_candidates+=("${candidate}")
        ;;
    esac
  done

  if (( ${#video_candidates[@]} == 0 )); then
    echo "[fatal] Nenhum video encontrado na pasta: ${VIDEO_PATH}" >&2
    return 1
  fi

  if (( ${#video_candidates[@]} == 1 )); then
    VIDEO_PATH="${video_candidates[0]}"
    return
  fi

  local selected_video
  echo "Escolha o numero do video que deseja usar:" >&2
  select selected_video in "${video_candidates[@]}"; do
    if [[ -n "${selected_video}" ]]; then
      VIDEO_PATH="${selected_video}"
      return
    fi
    echo "Opcao invalida. Informe um dos numeros listados." >&2
  done

  echo "[fatal] Nenhum video selecionado. Passe um arquivo no primeiro argumento ou escolha um numero." >&2
  return 1
}

select_video_from_directory

if [[ ! -f "${VIDEO_PATH}" ]]; then
  echo "[fatal] Video ou pasta de teste nao encontrado: ${VIDEO_PATH}" >&2
  show_usage >&2
  exit 1
fi

require_command ffmpeg
require_command ffplay
require_command cmake
require_command tee

if [[ ! -x "${MEDIAMTX_BIN}" ]]; then
  echo "[fatal] MediaMTX nao encontrado ou sem permissao de execucao: ${MEDIAMTX_BIN}" >&2
  echo "Voce pode sobrescrever o caminho com MEDIAMTX_BIN=/caminho/mediamtx" >&2
  exit 1
fi

if [[ -f "${ROOT_DIR}/mediamtx.yml" ]]; then
  MEDIAMTX_CONFIG="${ROOT_DIR}/mediamtx.yml"
elif [[ -f "${ROOT_DIR}/mediamtx.example.yml" ]]; then
  MEDIAMTX_CONFIG="${ROOT_DIR}/mediamtx.example.yml"
else
  echo "[fatal] Configuracao do MediaMTX nao encontrada." >&2
  exit 1
fi

mkdir -p "${RUN_DIR}"

printf '\n[1/4] Subindo MediaMTX...\n'
"${MEDIAMTX_BIN}" "${MEDIAMTX_CONFIG}" >"${RUN_DIR}/mediamtx.log" 2>&1 &
MEDIAMTX_PID=$!
sleep 1

if ! kill -0 "${MEDIAMTX_PID}" 2>/dev/null; then
  echo "[fatal] MediaMTX encerrou durante a inicializacao." >&2
  echo "Log: ${RUN_DIR}/mediamtx.log" >&2
  exit 1
fi

echo "      MediaMTX PID=${MEDIAMTX_PID}"

printf '\n[2/4] Publicando video de teste em loop...\n'
echo "      video=${VIDEO_PATH}"
echo "      rtsp=${RTSP_URL}"

ffmpeg \
  -hide_banner \
  -loglevel warning \
  -re \
  -stream_loop -1 \
  -i "${VIDEO_PATH}" \
  -an \
  -vf "scale=1920:1080" \
  -c:v h264_nvenc \
  -preset p4 \
  -b:v 5M \
  -maxrate 5M \
  -bufsize 10M \
  -g 60 \
  -f rtsp \
  "${RTSP_URL}" \
  >"${RUN_DIR}/ffmpeg.log" 2>&1 &
FFMPEG_PID=$!
sleep 2

if ! kill -0 "${FFMPEG_PID}" 2>/dev/null; then
  echo "[fatal] FFmpeg encerrou durante a publicacao." >&2
  echo "Log: ${RUN_DIR}/ffmpeg.log" >&2
  exit 1
fi

echo "      FFmpeg PID=${FFMPEG_PID}"

ffplay \
  -hide_banner \
  -loglevel warning \
  -fflags nobuffer \
  -flags low_delay \
  -framedrop \
  -window_title "TCC - video RTSP - MOG2" \
  "${RTSP_URL}" \
  >"${RUN_DIR}/ffplay.log" 2>&1 &
FFPLAY_PID=$!

echo "      FFplay PID=${FFPLAY_PID}"

printf '\n[3/4] Compilando detector MOG2...\n'
cmake \
  -S "${ROOT_DIR}/motion_mog2" \
  -B "${BUILD_DIR}" \
  -DOpenCV_DIR="${OPENCV_DIR}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"

printf '\n[4/4] Executando detector MOG2...\n'
echo "      Ctrl+C encerra detector, FFplay, FFmpeg e MediaMTX."
echo "      limiar=${MOTION_THRESHOLD_PERCENT}% inicio=${MOTION_START_FRAMES} frames fim=${MOTION_END_FRAMES} frames"
echo "      log=${RUN_DIR}/motion_mog2.log"
echo

"${BUILD_DIR}/motion_mog2" \
  "${RTSP_URL}" \
  "${MOTION_THRESHOLD_PERCENT}" \
  "${MOTION_START_FRAMES}" \
  "${MOTION_END_FRAMES}" \
  2>&1 | tee "${RUN_DIR}/motion_mog2.log"
