#!/bin/sh
set -eu

# Remove every installed component. Source and build output in the project
# directory are left alone. Run as root: sudo sh scripts/uninstall.sh

echo "Stopping agent"
/usr/bin/pkill -f 'HuishengStudio Agent.app/Contents/MacOS/HuishengStudio Agent' 2>/dev/null || true

echo "Removing HAL driver"
for name in HuishengStudio HuishengStudioIn12 HuishengStudioIn34 HuishengStudioIn56 HuishengStudioIn78 \
            HuishengStudioOut12 HuishengStudioOut34 HuishengStudioOut56 HuishengStudioOut78; do
    rm -rf "/Library/Audio/Plug-Ins/HAL/$name.driver"
done

echo "Removing plug-ins and agent"
# Both generations: 1.0.7 shipped the same two plug-ins under the old names, and
# an uninstall has to take those away too.
for d in VST3 Components; do
    ext=vst3
    [ "$d" = "Components" ] && ext=component
    for name in "HsAudio Capture" "HsAudio Return" \
                "HuishengStudio Capture" "HuishengStudio Return"; do
        rm -rf "/Library/Audio/Plug-Ins/$d/$name.$ext"
    done
done
rm -rf "/Applications/HuishengStudio Agent.app"

for home in /Users/*; do
    [ -d "$home" ] || continue
    rm -rf "$home/Applications/HuishengStudio Agent.app"
    for d in VST3 Components; do
        ext=vst3
        [ "$d" = "Components" ] && ext=component
        for name in "HsAudio Capture" "HsAudio Return" \
                    "HuishengStudio Capture" "HuishengStudio Return"; do
            rm -rf "$home/Library/Audio/Plug-Ins/$d/$name.$ext"
        done
    done
done

echo "Removing per-user shared memory"
rm -f /tmp/huishengstudio_*.ring /tmp/huishengstudio_loop_*.ring 2>/dev/null || true

CONSOLE_UID="$(/usr/bin/id -u "$(/usr/bin/stat -f %Su /dev/console)" 2>/dev/null || true)"
if [ -n "$CONSOLE_UID" ]; then
    for name in ctl tap ch channel ret; do
        /bin/launchctl asuser "$CONSOLE_UID" /bin/sh -c "rm -f /dev/null" 2>/dev/null || true
    done
    # shm objects live in /var/run; drop this user's namespace entries.
    ls /var/run/huishengstudio.* 2>/dev/null | while read -r f; do rm -f "$f"; done
fi

echo "Restarting core audio"
/bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null \
    || killall coreaudiod 2>/dev/null || true

/usr/sbin/pkgutil --forget com.huishengstudio.installer >/dev/null 2>&1 || true
echo "Done."
