#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# -static keeps libgcc/libstdc++/winpthread inside the exe so a stock
# Windows 10 machine does not need MinGW runtime DLLs.
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static \
  crossfade_player.cpp -o CrossFadePlayer.exe \
  -lstrmiids -lole32 -loleaut32 -lcomctl32 -lcomdlg32 \
  -lshell32 -lshlwapi -lgdi32 -luser32 -luuid
echo "Built $(pwd)/CrossFadePlayer.exe"
