#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

APP="$PWD/build/agent/HuishengStudio Agent.app"

if [ ! -d "$APP" ]; then
    echo "HuishengStudio Agent not built. Run scripts/build.sh first." >&2
    exit 1
fi

echo "Installing HuishengStudio Agent to ~/Applications"
mkdir -p "$HOME/Applications"
rm -rf "$HOME/Applications/HuishengStudio Agent.app"
/usr/bin/ditto "$APP" "$HOME/Applications/HuishengStudio Agent.app"

echo "Installed: $HOME/Applications/HuishengStudio Agent.app"
echo "The plug-ins start it automatically; macOS will ask for System Audio Recording"
echo "the first time an App is selected."
