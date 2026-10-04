#!/usr/bin/env bash
# Build the P0 probe with the MSYS2 MinGW64 toolchain.
#
# IMPORTANT: PATH must be confined to MSYS2. With the user's normal PATH,
# C:\Strawberry\c\bin\libwinpthread-1.dll shadows MSYS2's and cc1plus.exe dies
# with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139), producing no diagnostic at all.
set -euo pipefail

OUT="${1:-p0_probe.exe}"
SRC="$(dirname "$0")/p0_probe.cpp"

FLAGS="$(pkg-config --cflags mpv)"
LIBS="$(pkg-config --libs glfw3 mpv)"

echo "cflags: $FLAGS"
echo "libs  : $LIBS"

g++ -std=c++17 -O1 -g "$SRC" -o "$OUT" $FLAGS $LIBS -lopengl32 -lgdi32 -lole32
echo "built: $OUT"
