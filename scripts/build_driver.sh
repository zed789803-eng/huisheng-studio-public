#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

OUT="$PWD/build/drivers"
PLIST="$PWD/driver/Info.plist"
IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}"

rm -rf "$OUT"
mkdir -p "$OUT/HuishengStudio.driver/Contents/MacOS" "$OUT/HuishengStudio.driver/Contents/Resources"

clang++ -bundle -std=c++17 -O2 \
    -arch arm64 -arch x86_64 \
    -mmacosx-version-min=12.0 \
    -I "$PWD/shared" \
    driver/HuishengStudioDriver.cpp \
    -framework CoreAudio \
    -framework CoreFoundation \
    -o "$OUT/HuishengStudio.driver/Contents/MacOS/HuishengStudio"

cp "$PLIST" "$OUT/HuishengStudio.driver/Contents/Info.plist"
plutil -lint "$OUT/HuishengStudio.driver/Contents/Info.plist" >/dev/null

if security find-identity -v -p codesigning 2>/dev/null | grep -F "\"$IDENTITY\"" >/dev/null; then
    codesign --force --deep --sign "$IDENTITY" "$OUT/HuishengStudio.driver"
    echo "signed with '$IDENTITY'"
else
    # coreaudiod refuses an unsigned HAL bundle on current macOS, and CI has no
    # Developer ID: ad-hoc signing keeps the driver loadable for local builds.
    codesign --force --deep --sign - "$OUT/HuishengStudio.driver"
    echo "warning: signing identity '$IDENTITY' not found; ad-hoc signed" >&2
fi

echo "driver bundle: $OUT/HuishengStudio.driver"
