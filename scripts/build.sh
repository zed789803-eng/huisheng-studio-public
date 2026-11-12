#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

CMAKE="${CMAKE:-third_party/cmake-3.31.6-macos-universal/CMake.app/Contents/bin/cmake}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
BUILD="${BUILD:-build}"
CONFIG="${CONFIG:-Release}"

if [ ! -x "$CMAKE" ]; then
    # Fall back to a system cmake if the bundled one is absent.
    CMAKE="$(command -v cmake || true)"
fi

if [ -z "${CMAKE:-}" ]; then
    echo "cmake not found" >&2
    exit 1
fi

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}" \
"$CMAKE" -S . -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}" \
"$CMAKE" --build "$BUILD" -j "$JOBS"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}" \
sh cmake/postprocess_agent.sh "$BUILD/agent/HuishengStudio Agent.app"

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}" sh scripts/build_driver.sh

# The keygen app is the one the publisher keeps: give it an icon and a signature
# like every other bundle.
KEYGEN_APP="$(find "$BUILD" -type d -name 'HuishengStudio Keygen.app' 2>/dev/null | head -1)"

if [ -n "$KEYGEN_APP" ]; then
    CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}" \
        sh cmake/postprocess_keygen.sh "$KEYGEN_APP"
fi

echo
echo "built:"
echo "  $BUILD/agent/HuishengStudio Agent.app"
echo "  $BUILD/HuishengStudioCapture_artefacts/$CONFIG/VST3/HsAudio Capture.vst3"
echo "  $BUILD/HuishengStudioCapture_artefacts/$CONFIG/AU/HsAudio Capture.component"
echo "  $BUILD/HuishengStudioReturn_artefacts/$CONFIG/VST3/HsAudio Return.vst3"
echo "  $BUILD/HuishengStudioReturn_artefacts/$CONFIG/AU/HsAudio Return.component"
echo "  $BUILD/drivers/HuishengStudio.driver"
echo "  $BUILD/hsmon"
echo "  $BUILD/hskeygen"
[ -n "$KEYGEN_APP" ] && echo "  $KEYGEN_APP"
