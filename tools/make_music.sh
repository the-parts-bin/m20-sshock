#!/usr/bin/env bash
# Renders System Shock's music to the WAV files the port streams (see
# ../README.md, Music): every sequence of the themes in SOUND/SBLASTER/
# (THM*.XMI, TITLOOP, ENDLOOP, END, INTROTHM) on its own, as the music AI
# starts them one by one, layering several, through libADLMIDI
# (../core/tools/midirender: Nuked OPL3, the game's AdLib timbres CIT.AD,
# Miles' volume model), then 24 kHz 16-bit mono with one gain per theme (the
# layers keep their balance: the theme's loudness to -18 LUFS, its loudest
# peak at most -1 dBFS). Silent sequences (the themes' control sequence 0,
# placeholders) are left out: a missing file plays nothing. Files are
# <THEME>_<N>.WAV; optionally copied to the SD card (SSHOCK/MUSIC/) with
# ../../core/tools/sdput/sdput.py.
#
# Usage: make_music.sh SSHOCKDIR OUTDIR [--upload | --to CARDDIR]   (SSHOCKDIR holds SOUND/)
# Needs python3, ffmpeg, git, cmake and a C/C++ compiler.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
case "${3-}" in   # --upload: to the board's SD card; --to: into a card's file tree
"" | --upload) PUT=(put) ;;
--to) PUT=(put --to "${4:?--to needs a directory}") ;;
*) echo "unknown option $3" >&2; exit 1 ;;
esac
SB=$1/SOUND/SBLASTER
AD=$1/SOUND/CIT.AD
OUT=$(realpath -m "$2")
HERE=$(dirname "$(realpath "$0")")
MR=$HERE/../../core/tools/midirender
mkdir -p "$OUT/raw" "$OUT/wav"

"$MR/build.sh" >/dev/null
for f in "$SB"/*.XMI; do
    b=$(basename "$f" .XMI)
    n=$(python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(); i=d.find(b"INFO"); print(struct.unpack_from("<H",d,i+8)[0] if i>=0 else 1)' "$f")
    for ((t = 0; t < n; t++)); do
        echo "$MR/build/midirender $f $OUT/raw/${b}_$t.wav -t $t -b 45 -a $AD -v ail -s 600"
    done
done | xargs -P "$(nproc)" -I{} sh -c '{} >/dev/null 2>&1'

python3 - "$OUT" "$MR" <<'PY'
import array, glob, os, subprocess, sys
out, mr = sys.argv[1], sys.argv[2]
sys.path.insert(0, mr)
from finish import measure
themes = {}
for f in sorted(glob.glob(f"{out}/raw/*.wav")):
    d = open(f, "rb").read()[44:]
    a = array.array("f"); a.frombytes(d[:len(d) // 4 * 4])
    if a and max(map(abs, a)) > 0:
        themes.setdefault(os.path.basename(f).rsplit("_", 1)[0], []).append(f)
for theme, files in sorted(themes.items()):
    lst = f"{out}/raw/{theme}.txt"
    open(lst, "w").write("".join(f"file '{f}'\n" for f in files))
    cat = f"{out}/raw/{theme}.all.wav"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
                    "-c", "copy", cat], check=True)
    loud, peak = measure(cat)
    gain = min(-18.0 - loud, -1.0 - peak)
    print(f"{theme}: {len(files)} pieces, {loud:.1f} LUFS, peak {peak:.1f} dBFS, gain {gain:+.1f} dB")
    for f in files:
        name = os.path.basename(f)[:-4].upper() + ".WAV"
        subprocess.run([sys.executable, f"{mr}/finish.py", f, f"{out}/wav/{name}", "--gain", f"{gain:.2f}"],
                       check=True, capture_output=True)
    os.remove(cat)
PY
du -sh "$OUT/wav"
if [ -n "${3-}" ]; then
    args=()
    for f in "$OUT"/wav/*.WAV; do args+=("$f" "SSHOCK/MUSIC/$(basename "$f")"); done
    python3 "$HERE/../../core/tools/sdput/sdput.py" "${PUT[@]}" "${args[@]}"
fi
