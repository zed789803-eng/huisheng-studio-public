#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

BUNDLE="$PWD/build/drivers/HuishengStudio.driver"
TARGET="/Library/Audio/Plug-Ins/HAL/HuishengStudio.driver"

if [ ! -d "$BUNDLE" ]; then
    echo "No driver bundle. Run scripts/build_driver.sh first." >&2
    exit 1
fi

echo "Installing huishengstudio virtual audio devices."
echo "System audio will be interrupted for a moment. Save your DAW session first."

sudo mkdir -p /Library/Audio/Plug-Ins/HAL

# The previous generation installed one bundle per direction, and the 1.0.8
# generation installed one duplex bundle. Remove them so this bundle's devices
# are the only huishengstudio devices left.
for old in HuishengStudioIn12 HuishengStudioIn34 HuishengStudioIn56 HuishengStudioIn78 \
           HuishengStudioOut12 HuishengStudioOut34 HuishengStudioOut56 HuishengStudioOut78; do
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/$old.driver"
done

sudo rm -rf "$TARGET"
sudo /usr/bin/ditto "$BUNDLE" "$TARGET"

# coreaudiod only loads HAL plug-ins that it can trust: the bundle must be
# owned by root. ditto preserves the build tree's ownership (your account), so
# fix it here - this is the step that makes the devices show up.
sudo /usr/sbin/chown -R root:wheel "$TARGET"
sudo /bin/chmod -R go-w "$TARGET"
sudo /usr/bin/xattr -cr "$TARGET" 2>/dev/null || true
sudo /usr/bin/codesign --verify --deep --strict "$TARGET"

sudo /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null \
    || sudo /usr/bin/killall coreaudiod 2>/dev/null || true

sleep 2

echo "Installed. The output tab should list four devices, and the input tab four more:"
echo "  output:  HsAudioSC 1/2   HsAudioSC 3/4   HsAudioSC 5/6   HsAudioSC 7/8"
echo "  input:   HsAudioSR 1/2   HsAudioSR 3/4   HsAudioSR 5/6   HsAudioSR 7/8"
echo
echo "If they are still missing, run: sh scripts/doctor.sh"
