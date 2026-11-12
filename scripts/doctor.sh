#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

BIN="${1:-build/hsdoctor}"

if [ ! -x "$BIN" ]; then
    CMAKE="${CMAKE:-third_party/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake}"
    [ -x "$CMAKE" ] || CMAKE="$(command -v cmake)"
    "$CMAKE" --build build -j "$(sysctl -n hw.ncpu)" --target hsdoctor >/dev/null
fi

"$BIN"
