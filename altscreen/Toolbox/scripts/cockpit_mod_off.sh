#!/bin/sh
# MMI-Cockpit-Carplay: OFF = stock CarPlay (no RGI hook, no AltScreen, no mirror),
# ON = the mod as configured. Applies on the next phone connection.
set -u
OFF_FILE=/mnt/app/root/hooks/mod.off
[ -d /mnt/app/root/hooks ] || { echo "FAIL: RGI is not installed (no /mnt/app/root/hooks)"; exit 1; }
mount -uw /mnt/app || { echo "FAIL: cannot mount /mnt/app writable"; exit 1; }
: > "$OFF_FILE" || { mount -ur /mnt/app; echo "FAIL: cannot write $OFF_FILE"; exit 1; }
sync
mount -ur /mnt/app
echo "COCKPIT_MOD=OFF. Reconnect the phone to apply."
