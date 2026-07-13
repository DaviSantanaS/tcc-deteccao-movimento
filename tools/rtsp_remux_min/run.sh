#!/usr/bin/env bash
set -euo pipefail
RTSP_URL="${1:-rtsp://127.0.0.1:8554/video}"
OUT_DIR="${2:-$HOME/tcc/captures}"
mkdir -p "$OUT_DIR"
STAMP=$(date +"%Y%m%d_%H%M%S")
OUT_FILE="${OUT_DIR}/capture_${STAMP}.mp4"
echo "➡️  Remuxando ${RTSP_URL} -> ${OUT_FILE}"
~/tcc/tools/rtsp_remux_min/rtsp_remux_min "${RTSP_URL}" "${OUT_FILE}"
echo "✅ Arquivo salvo em: ${OUT_FILE}"
