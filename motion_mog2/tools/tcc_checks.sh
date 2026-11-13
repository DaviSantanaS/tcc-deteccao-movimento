#!/usr/bin/env bash
# Uso (a partir da raiz do repositorio): source motion_mog2/tools/tcc_checks.sh

export OPENCV_DIR="${OPENCV_DIR:-/home/davi/Downloads/tcc/opencv/install-cuda125/lib/cmake/opencv4}"
export VIDEO="/home/davi/Vídeos/tcc/video_h264_fhd_60fps.mp4"
export BUILD=motion_mog2/build
export BIN=$BUILD/motion_mog2
export MEDIAMTX_BIN="${MEDIAMTX_BIN:-/home/davi/Downloads/tcc/mediamtx}"
export RTSP_URL=rtsp://127.0.0.1:8554/video

tcc_md5() { ffmpeg -v error -i "$1" -map 0:v:0 -c copy -f framemd5 - | grep -v '^#' | awk -F', *' '{print $NF}'; }

# uso: tcc_ranges <video_fonte> <diretorio_dos_mp4>   -> uma linha RANGE por MP4
tcc_ranges() {
  local source_video="$1" output_directory="$2" recording first_hash packet_count first_index last_index
  tcc_md5 "$source_video" > /tmp/tcc_source.md5
  for recording in "$output_directory"/*.mp4; do
    tcc_md5 "$recording" > /tmp/tcc_recording.md5
    packet_count=$(wc -l < /tmp/tcc_recording.md5)
    first_hash=$(head -n 1 /tmp/tcc_recording.md5)
    first_index=$(( $(grep -n -m 1 "$first_hash" /tmp/tcc_source.md5 | cut -d: -f1) - 1 ))
    last_index=$(( first_index + packet_count - 1 ))
    if [ "$(sed -n "$((first_index + 1)),$((last_index + 1))p" /tmp/tcc_source.md5 | md5sum)" = "$(md5sum < /tmp/tcc_recording.md5)" ]; then
      echo "RANGE $(basename "$recording") [$first_index,$last_index] packets=$packet_count contiguous=yes"
    else
      echo "RANGE $(basename "$recording") first=$first_index packets=$packet_count contiguous=NO"
    fi
  done
}

# uso: tcc_check_mp4 <arquivo.mp4>   -> uma linha MP4CHECK
tcc_check_mp4() {
  local first_flags decode_error_lines counts
  first_flags=$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 -read_intervals '%+#1' "$1" | head -n 1)
  # Avisos do muxer null sobre a saida de descarte nao sao erros de decodificacao.
  decode_error_lines=$(ffmpeg -v error -i "$1" -f null - 2>&1 | awk '!/^\[null @/ { error_count++ } END { print error_count + 0 }')
  counts=$(ffprobe -v error -select_streams v:0 -count_packets -count_frames -show_entries stream=nb_read_packets,nb_read_frames -of default=nw=1 "$1" | tr '\n' ' ')
  echo "MP4CHECK $(basename "$1") first_packet_flags=$first_flags decode_error_lines=$decode_error_lines $counts"
}

# uso: tcc_normalize_log <arquivo.log>   -> log sem os caminhos dos MP4 (o nome do arquivo contém a hora)
tcc_normalize_log() { sed -E 's/path="[^"]*"/path=X/g' "$1"; }
