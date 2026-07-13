#!/usr/bin/env bash
set -euo pipefail

# Dependências (Ubuntu/Debian):
#   sudo apt update
#   sudo apt install -y pkg-config libavformat-dev libavcodec-dev libavutil-dev

CXX=${CXX:-g++}
CXXFLAGS="-O2 -std=c++17"

${CXX} ${CXXFLAGS} \
  ~/tcc/tools/rtsp_remux_min/rtsp_remux_min.cpp -o ~/tcc/tools/rtsp_remux_min/rtsp_remux_min \
  $(pkg-config --cflags --libs libavformat libavcodec libavutil)

echo "✅ Build ok: ~/tcc/tools/rtsp_remux_min/rtsp_remux_min"
