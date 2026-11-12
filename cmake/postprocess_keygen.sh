#!/bin/sh
set -e

APP="$1"
PLIST="$APP/Contents/Info.plist"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "$PLIST" ]; then
    echo "postprocess_keygen: missing $PLIST" >&2
    exit 1
fi

set_key() {
    /usr/libexec/PlistBuddy -c "Delete :$1" "$PLIST" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$1 $2 $3" "$PLIST"
}

set_key NSPrincipalClass string NSApplication
# The binary itself is built for 12.0; say so, or LaunchServices has no way to
# tell the user why it will not start on 11.
set_key LSMinimumSystemVersion string 12.0
set_key CFBundleName string 绘声Studio注册码生成器
set_key CFBundleDisplayName string 绘声Studio注册码生成器

ICON="$APP/Contents/Resources/AppIcon.icns"
mkdir -p "$APP/Contents/Resources"

if [ -f "$ROOT/ui/logo.png" ]; then
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/hs-keygen-icon.XXXXXX")"
    ICONSET="$TMP/AppIcon.iconset"
    mkdir -p "$ICONSET"
    for size in 16 32 128 256 512; do
        /usr/bin/sips -z "$size" "$size" "$ROOT/ui/logo.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        /usr/bin/sips -z "$double" "$double" "$ROOT/ui/logo.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
    done
    /usr/bin/iconutil -c icns "$ICONSET" -o "$ICON"
    rm -rf "$TMP"
    set_key CFBundleIconFile string AppIcon.icns
    set_key CFBundleIconName string AppIcon
fi

IDENTITY="${CODESIGN_IDENTITY:-HuishengStudio Dev}"

if security find-identity -v -p codesigning 2>/dev/null | grep -F "\"$IDENTITY\"" >/dev/null; then
    /usr/bin/codesign --force --deep --sign "$IDENTITY" "$APP"
    echo "postprocess_keygen: signed with '$IDENTITY'"
else
    echo "postprocess_keygen: identity '$IDENTITY' not found; ad-hoc signing" >&2
    /usr/bin/codesign --force --deep --sign - "$APP"
fi
