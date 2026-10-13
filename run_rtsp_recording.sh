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
