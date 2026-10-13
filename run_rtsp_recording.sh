#!/usr/bin/env bash
set -euo pipefail

show_usage() {
  printf '%s\n' \
    'Uso: bash run_rtsp_recording.sh [arquivo_h264] [diretorio_saida] [leituras_por_trecho]' \
    'Padroes: ~/Vídeos/tcc/video_h264_fhd_60fps.mp4, recordings do projeto, 1000 leituras.' \
    'RTSP local: rtsp://127.0.0.1:8554/video; corte dos MP4s no proximo keyframe.' \
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
