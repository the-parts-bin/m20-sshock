#!/usr/bin/env bash
# Copies System Shock's data from an installed CD version (e.g. Steam's
# "System Shock: Classic", SS1/SSHOCK) to the SD card in the board, with
# ../../core/tools/sdput/sdput.py (about 64 KB/s): SSHOCK/RES/DATA/, the
# files the m9b build reads (English; the low resolution movies), and
# SSHOCK/RES/SOUND/THM*.BIN, the music AI's tables. The audio logs and
# barks (CITALOG.RES, CITBARK.RES: 61 MB of the 97) can be left out with
# --no-logs (emails then show their text without the audio): about 9
# minutes instead of 25. The music itself: tools/make_music.sh. --list
# only prints what would be copied; --to CARDDIR copies into a card's tree.
#
# Usage: put_data.sh SSHOCKDIR [--no-logs] [--list] [--to CARDDIR]
#   SSHOCKDIR holds DATA/, e.g.
#   ~/.local/share/Steam/steamapps/common/SS1/SSHOCK
set -euo pipefail

if [ $# -lt 1 ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
SRC=$1/DATA
shift
NO_LOGS=0
LIST=0
PUT=(put)
while [ $# -gt 0 ]; do
    case $1 in
    --no-logs) NO_LOGS=1 ;;
    --list) LIST=1 ;;
    --to) PUT=(put --to "${2:?--to needs a directory}"); shift ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift
done
HERE=$(dirname "$(realpath "$0")")

# Everything in DATA/ but the other languages (FRN*, GER*, LOFR*, LOGE*)
# and the SVGA movies (SV*).
args=()
total=0
for f in "$SRC"/*; do
    b=$(basename "$f")
    case ${b^^} in
    FRN* | GER* | LOFR* | LOGE* | SV*) continue ;;
    CITALOG.RES | CITBARK.RES) [ $NO_LOGS = 1 ] && continue ;;
    esac
    args+=("$f" "SSHOCK/RES/DATA/${b^^}")
    total=$((total + $(stat -c %s "$f")))
done
for f in "${SRC%/DATA}"/SOUND/THM*.BIN "${SRC%/DATA}"/SOUND/thm*.bin; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    args+=("$f" "SSHOCK/RES/SOUND/${b^^}")
    total=$((total + $(stat -c %s "$f")))
done
if [ ${#args[@]} -eq 0 ]; then
    echo "no files in $SRC" >&2
    exit 1
fi
echo "$((${#args[@]} / 2)) files, $((total / 1048576)) MB, about $((total / 65536 / 60 + 1)) minutes"
if [ $LIST = 1 ]; then
    printf '%s -> %s\n' "${args[@]}"
    exit 0
fi
python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${args[@]}"
