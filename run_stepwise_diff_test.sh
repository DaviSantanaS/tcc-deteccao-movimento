#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VIDEO_PATH="${1:-${ROOT_DIR}/video/source_timer.mp4}"
RTSP_URL="${RTSP_URL:-rtsp://127.0.0.1:8554/video}"
OPENCV_DIR="${OPENCV_DIR:-/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4}"
MEDIAMTX_BIN="${MEDIAMTX_BIN:-/home/davi/Downloads/tcc/mediamtx}"
BUILD_DIR="${ROOT_DIR}/cpp_motion_headless_diff/build"
RUN_DIR="${ROOT_DIR}/.run_stepwise_diff"

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

trap cleanup EXIT INT TERM

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[fatal] Comando nao encontrado: $1" >&2
    exit 1
  fi
}

require_command ffmpeg
require_command ffplay
require_command cmake

if [[ ! -f "${VIDEO_PATH}" ]]; then
  echo "[fatal] Video de teste nao encontrado: ${VIDEO_PATH}" >&2
  echo "Uso: bash run_stepwise_diff_test.sh [caminho-do-video.mp4]" >&2
  exit 1
fi

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

# Visualizacao separada apenas para acompanhar o teste.
# O detector de diferenca de pixels continua lendo e processando o RTSP de forma independente.
ffplay \
  -hide_banner \
  -loglevel warning \
  -fflags nobuffer \
  -flags low_delay \
  -framedrop \
  -window_title "TCC - video RTSP - Pixel Diff" \
  "${RTSP_URL}" \
  >"${RUN_DIR}/ffplay.log" 2>&1 &
FFPLAY_PID=$!

echo "      FFplay PID=${FFPLAY_PID}"

printf '\n[3/4] Compilando detector PIXEL_DIFF...\n'
cmake \
  -S "${ROOT_DIR}/cpp_motion_headless_diff" \
  -B "${BUILD_DIR}" \
  -DOpenCV_DIR="${OPENCV_DIR}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"

printf '\n[4/4] Executando detector PIXEL_DIFF...\n'
echo "      Ctrl+C encerra detector, FFplay, FFmpeg e MediaMTX."
echo

"${BUILD_DIR}/motion_headless_diff" "${RTSP_URL}"
