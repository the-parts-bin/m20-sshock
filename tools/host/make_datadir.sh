#!/usr/bin/env bash
# Makes a data directory for the host harness (sshock-host DATADIR ...): the
# card's SSHOCK/ layout (res/data/, res/sound/) as symbolic links with
# lower-case names to an installed CD version's files (Linux is
# case-sensitive, FatFs isn't).
#
# Usage: make_datadir.sh SSHOCKDIR DATADIR
#   SSHOCKDIR holds DATA/ and SOUND/, e.g.
#   ~/.local/share/Steam/steamapps/common/SS1/SSHOCK
set -euo pipefail

if [ $# -ne 2 ]; then
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
SRC=$(realpath "$1")
DST=$2
link_dir() {   # link_dir FROM TO: each file, recursively, lower-cased
    mkdir -p "$2"
    for f in "$1"/*; do
        local b
        b=$(basename "$f" | tr '[:upper:]' '[:lower:]')
        if [ -d "$f" ]; then link_dir "$f" "$2/$b"; else ln -sf "$f" "$2/$b"; fi
    done
}
link_dir "$SRC/DATA" "$DST/res/data"
[ -d "$SRC/SOUND" ] && link_dir "$SRC/SOUND" "$DST/res/sound"
echo "OK: $DST"
