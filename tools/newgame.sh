#!/usr/bin/env bash
# Loads a build (default build-prof/), skips the intro and starts a new
# game (level 1, normal difficulty) from the UART's keys, waits for the
# level to load and takes a screenshot (/tmp/shock.png). For tests (the
# SD card must hold the data: put_data.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build-prof}
python3 ../core/tools/run.py "$B/sshock.bin" --listen 25 > /dev/null 2>&1
# Esc skips the intro; Down, Enter: New Game; a name, Enter: start.
python3 ../core/tools/m9bctl.py keys "<esc><wait:3000><down><wait:500><enter><wait:2000>m9b<wait:500><enter>"
sleep 30
python3 ../core/tools/m9bctl.py keys "<space>"
sleep 5
python3 ../core/tools/m9bctl.py shot /tmp/shock.png --scale 2
