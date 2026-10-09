#!/bin/sh
# MMI-Cockpit-Carplay: wired = AltScreen CarPlay map on the cluster (cable iPhone),
# wireless = main-screen mirror (adapter, default). Applies on the next phone connection.
set -u
MODE_FILE=/mnt/app/root/hooks/mode.wired
[ -d /mnt/app/root/hooks ] || { echo "FAIL: RGI is not installed (no /mnt/app/root/hooks)"; exit 1; }
mount -uw /mnt/app || { echo "FAIL: cannot mount /mnt/app writable"; exit 1; }
: > "$MODE_FILE" || { mount -ur /mnt/app; echo "FAIL: cannot write $MODE_FILE"; exit 1; }
sync
mount -ur /mnt/app
echo "COCKPIT_MODE=wired. Reconnect the iPhone (cable) to apply."
