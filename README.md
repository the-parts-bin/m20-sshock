# System Shock on the m20 SoC

System Shock (1994) as a program for the SoC in `../core`: Shockolate,
the cross-platform C port of the source Night Dive Studios released in
2018, with an m9b/m20 platform layer in place of its SDL one. It runs from
SDRAM, uses picolibc, and reads the CD version's data from the SD card
through the core firmware's SD driver and FatFs.

**m20:** this is the m9b port (m9b-sshock at `c2743e2`) moved to the m20:
the m20's CPU flags (the F extension, Zicboz), its clock (54 MHz, from
`../core/firmware/clock.h`), its flash disk driver, and video for its
1280 x 720 picture (720 lines; columns doubled by the line table and, at
320 wide, once more in the copy). It starts in the game's SVGA mode,
640 x 480 (`MacSrc/Prefs.c`; 320 x 200 can still be chosen in the game's
options or `SSHOCK/prefs.txt`), and the span engine
(`../core/rtl/span_engine.v`) draws the 3D view's walls, floors and
ceilings (below).

## The span engine

`port/m20_engine.c` takes the main view's texture-mapped polygons whose
bitmap is one of the level's textures (`draw_tmap_common`,
`3D/Source/tmap.c`) and has the span engine draw them, RGB565 with
perspective-correct texturing, straight into the frame shown, at the
view's place on the screen; the CPU draws everything else (objects,
sprites, the overlays, the interface) into the game's 8 bpp canvases as
before. The view's canvas is cleared to colour 255 (the game's clear
colour, a hole), and the engine clears its own view at the frame's start.
What the CPU draws in the view is tracked as up to eight boxes (the 3D
library's five ways into the 2D drawing, voxels as the whole view); when
an engine polygon comes over a box, the engine is let finish, the box's
pixels go onto the engine's frame through the palette and are left as
holes, and the engine goes on: the painter's order is kept without a
memset under every span. Where the game copies the view canvas to the
screen, the rest (later CPU drawing, the overlays, the mouse) goes onto
the engine's frame in one pass instead, which leaves the canvas all holes,
so the next frame's clear is left out. The polygon's rows come from the
engine's EDGE command (a header per edge pair, Shock's pixel rule:
x + 0xFFFF, the window cl .. cr - 1); the light per vertex (one of the
shading table's 16 rows) becomes a small lightmap, each row matched as one
brightness factor. RSD (run-length) bitmaps the view maps are unpacked
once, a few kept (192 KB). The textures are
cleaned from the data cache and their colours moved off 224..255 (which
the engine leaves unlit) when a level's textures are loaded (`textmaps.c`).

Memory decides the frame's layout: the heap had about 130 KB to spare at
640 x 480, so whole RGB565 frames (three of 614 KB) don't fit. A display
buffer holds just the rows the view covers (259), RGB565; the line table
shows the other rows straight from the game's screen (8 bpp, the
interface, which rarely changes), and the interface columns beside the
view are converted only when they change (a checksum per row and buffer).
Frames without the engine (menus, cutscenes) are shown 8 bpp as before.
The engine is used at 640 x 480 with the view in its slot (not in the
full-screen view or at 320 x 200).

Ctrl-K on the terminal cycles the engine: all, walls only (the game's
vertical wall mapper), floors and the rest, off. Ctrl-D sends the 8 bpp
screen as the game drew it (with its holes), for debugging. Ctrl-R (tests)
has the CPU make the rows instead of EDGE; Ctrl-O (tests) turns the
on-line help off and on without saving it. The profile build (below) also
prints the engine's counters every 5 s; `EXTRA=-DM20_CHECK` reports CPU
drawing that wasn't put in a box.

Measured on the board (2026-11-08, level 1's first room, `make
B=build-prof`):

| 640 x 480 | fps |
|---|---|
| the CPU (the m9b port's mappers) | 5.2 |
| the span engine | 10.2 |
| (320 x 200, the CPU) | 11.6 |

2026-11-14, the same room with the on-line help off (its pick-up render
costs about 2.5 fps while it's on):

| step | fps |
|---|---|
| the span engine, holes under every span | 12.9 |
| the CPU's drawing composed in boxes, no holes | 13.8 |
| one pass at the copy to the screen, no clear | 15.7 |
| RSD bitmaps unpacked once | 16.2 |
| several boxes, composed only under engine polygons | 16.7 |
| EDGE, the lightmap stepped in fixed point | 18.1 |

What's left is mostly the CPU's: each engine polygon's setup, the pass at
the copy to the screen (about 10%: it reads the whole view canvas), the
boxes (about 6%, half of it waiting for the engine), the game. (On
2026-11-08:) The engine is under 40% busy, with
many texel cache misses on the 128 x 128 textures seen at an angle (its
cache is 2 KB, direct-mapped); giving it the 64 x 64 versions halved the
misses but gained only 3%. Open: now and then `ResLock: Could not load
112` (not seen without the engine's buffers, which take 93 KB more than
the 8 bpp ones did).

**Status:** it plays on the m20 board at 640 x 480 with the span engine
(above), with sound effects and the music (the themes' sequences,
pre-rendered: Music). (On the m9b it played at about 7.4 fps in a saved
game, 320 x 200 on the CPU.) In the host test harness (`tools/host/`, the same
game and port code on 32-bit x86 Linux, held to the board's heap) it
plays too: the splash screens, the intro, the menus, a new game, walking
around level 1, sound effects, saving.

Start it from the boot screen's browser (`SSHOCK.BIN` on the card), or with
`../core/tools/run.py build/sshock.bin`. On the first start (no
`SSHOCK/prefs.txt` yet) the intro plays; Esc skips it.

**Input:** USB keyboards and mice on either USB port, with the game's
controls (Shockolate's defaults: mouse and WASD; `?` lists the keys);
the terminal's keys work too (arrows, Enter, Esc, Tab, Backspace,
printable characters), which is enough for the menus (Down, Enter: New
Game; a name, Enter: start).

**Double clicks** need two fixes at this frame rate. The buttons are
sampled in the audio interrupt with their times (the main loop polls once a
frame, 100 ms or more apart, so a quick double click fell between two
polls). And the UI expires a pending click at the time the input was
collected (`m9b_input_time`), not at the current time: the main loop polls
the UI before it pumps the input, a frame after the last pump, so the
first click expired while the second, made in time, was still queued. At
7 fps (140 ms per frame, the host harness with its input on a 5 ms timer
like the interrupt) no double click of ten registered before, and ten of
ten after. Also, the pointer moves one screen pixel per mouse count,
so the game's double-click tolerance (how far the pointer may move between
the clicks) is raised from 5 pixels to 16 (`port/m9b_shock.c`): at 5, a
tiny hand movement broke most double clicks. How quickly the second click
has to follow is the game's own setting (Options, Input: Double click).

**License:** GPL v3 (`LICENSE`, a copy of `shockolate/LICENSE`), the port included. The game data isn't free and
isn't in the repository; it comes from an installed CD version (e.g.
Steam's "System Shock: Classic", whose `SS1/SSHOCK` is the CD install).

## Sources

| Part | Upstream | Version | Here |
|---|---|---|---|
| The game | [Interrupt/systemshock](https://github.com/Interrupt/systemshock) (Shockolate: the portable C port of the source Night Dive Studios released) | `6c1fe472` | `shockolate/`, trimmed, with changes (Changes to Shockolate) |
| SDL stand-in | [SDL2](https://github.com/libsdl-org/SDL) and SDL_mixer's API, for the parts the game still uses | | `port/SDL.h`, `port/SDL_mixer.h`, written for the port |
| Music renderer | [Wohlstand/libADLMIDI](https://github.com/Wohlstand/libADLMIDI) | `ef98fab` (`../core/tools/midirender/build.sh`) | used by `tools/make_music.sh` on the PC |

## Why Shockolate

Night Dive's release is the 1995 Macintosh PowerPC source, with Mac
Toolbox calls and some 68K/PowerPC assembly. Shockolate (upstream commit
`6c1fe472`) is that source made portable: the assembly in C, the Mac
parts on SDL2, the DOS CD data read directly. Its physics engine (EDMS) and
fixed-point class are C++ without the library (no streams or exceptions in
the code the game uses), so they build with picolibc's C++ support and no
libstdc++.

## Layout

| Path | Contents |
|---|---|
| `shockolate/` | Shockolate, trimmed to what the build uses (no OpenGL renderer, MIDI synthesizers, CMake, Mac archive, Windows and macOS packaging), with changes (below) |
| `port/m9b_shock.c` | in place of `MacSrc/Shock.c` and `ShockBitmap.c`: `main`, video (surfaces, display buffers, palette, the vertical-blank flip), time, screenshots over the UART |
| `port/m9b_events.c` | in place of `INPUT/Source/sdl_events.c`: USB keyboards and mice and the UART's keys into the game's key and mouse event queues (HID usages to Mac key codes) |
| `port/m9b_sound.c` | in place of `MacSrc/SDLSound.c` and `Xmi.c`: the effects mixer (VOC samples), the audio stream cutscenes and audio logs feed, the music (pre-rendered sequences, streamed) |
| `port/m9b_posix.c` | files under `SSHOCK/` (the linker wraps `open`, `unlink`, `rename`), `access`, `gettimeofday`, `stdin` |
| `port/m9b_hw.h` | interrupts, cache cleaning, start-up (no-ops in the host build) |
| `port/SDL.h`, `SDL_mixer.h`, `shockolate_version.h` | a stand-in for the SDL2 types and calls the game code still makes; the header upstream's CMake generates |
| `tools/put_data.sh` | copies the data from an installed CD version to the card |
| `tools/make_music.sh` | renders the music to WAV files on a PC (see Music) and optionally copies them to the card |
| `tools/newgame.sh` | loads a build and starts a new game from the UART (tests) |
| `tools/host/` | the host test harness: `host.c`, `make_datadir.sh` |
| `../core/runtime/syscalls.c`, `string.c`, `trap.c`, `trap_entry.S` | the runtime all the ports share (in the core): files through FatFs, the UART console, misaligned access emulation, the trap handler and PC-sampling profiler |

## Building and the SD card

`make` builds `build/sshock.bin` (about 1 MB). `make B=build-prof
EXTRA="-DM9B_PROFILE -DPROF_SIZE=0x00100000u"` reports frames per second,
sound, frame copy and file read time, emulated misaligned accesses and the
heap's top every 5 s, and the PC profile every 15 s
(`../core/tools/prof.py ELF LOG`). Also in `EXTRA`: `-DPROF_ON_AUDIO`
samples on the audio interrupts (187.5 Hz, not tied to the display) instead
of the vertical blank; `-DPROF_MEMCPY_CALLERS=<memcpy's size>` counts
`memcpy`'s samples at its callers; `-DM9B_DIV_STATS` counts the fixed-point
divisions by path (one hardware divide, the 64-by-32-bit routine, libgcc).
The first 15 s window includes the menus (which redraw at 200 fps):
leave it out when looking at the game. With the engine the report adds the
frames by kind, the composing's time and the engine's counters.

The m9b port's speed-ups (5.8 to 6.9 fps in a saved game, then 7.4 with the
core's radix-4 divider), which the m20 keeps: the texture unpacker (`RSD/RSDUnpack.c`, M9B)
without unaligned word accesses (97% of the misaligned accesses the trap
handler emulated), and the fixed-point divisions (`FIX/Source/fix.c`)
through the hardware divider (`../core/firmware/m9b_div.h`) instead of
libgcc's `__divdi3`.

The game data goes in `data/` (`data/README.md` says what goes there).
The whole card, all the games with their data and music, is built in
m20-system, which has this repository as a submodule:
`../sdcard/make_sdcard.sh` takes the data from here. The scripts below add or replace files on a card in the board,
through the serial port.

The card needs `SSHOCK/RES/DATA/`: `tools/put_data.sh
~/.local/share/Steam/steamapps/common/SS1/SSHOCK` copies the 38 files the
build reads (92 MB, about 20 minutes through the board; `--no-logs` leaves
out the audio logs and barks: 34 MB, 7 minutes; emails then show their
text without the audio) and `SSHOCK/RES/SOUND/THM*.BIN`, the music AI's
tables; the music itself is `tools/make_music.sh` (Music); and `SSHOCK.BIN` in the
root for the boot screen's browser (`../core/tools/sdput/sdput.py put
build/sshock.bin SSHOCK.BIN`). The game writes `prefs.txt`, `keybinds.txt`,
`CurrentGame.dat` and the saves (`savgam00.dat` ...) to `SSHOCK/`.

## Memory

SDRAM (`../core/runtime/m9b.ld`): the image (1 MB) from `0x800000`, then
data and 2 MB of bss, the heap (5.1 MB; it was 3.7 MB on the m9b when
programs loaded at +1 MB) and a 128 KB stack at the top. At 640 x 480 the
heap's top reaches about 4.9 MB, with the display buffers sized to fit
(The span engine).
Shockolate, like the Mac version it comes from, assumed much more:
starting level 1 took 5.3 MB of heap, and a cutscene 17 MB. Now level 1
takes 2.8 MB and a cutscene about 0.2 MB:
- **Resource table:** one 34-byte entry for each of the 32768 possible
  resource ids (1.1 MB); System Shock's go up to $15B1, so 8192 (it grows if
  need be).
- **Compound resources** (a texture's sizes, a critter's frames, ...) were
  read whole when one item was asked for, and the item decoded into a copy
  next to it. Now only the ref table is read, and each item from the file
  when first asked for (`RES/Source/refacc.c`, `resload.c`; the 13
  compressed ones in `MFDART.RES` are still read whole).
- **Movies** (intro, death, ending, and the audio logs, which are movies
  without pictures) were copied into memory whole (14 MB for the intro) and
  their sound unpacked into another buffer (2.5 MB). Now they're read from
  the file as they play, the video and the sound each through its own file
  descriptor (`AFILE/Source/movie.c`, `amov.c`), and the sound fed to the
  mixer a block (8 KB) at a time from an 8-block ring. They're also freed
  afterwards (upstream kept every one played). The cutscenes are the low
  resolution ones (320 x 150, `LOW*.RES`, as DOS's 320 x 200 mode had),
  not the 600 x 300 SVGA ones: a quarter of the decoding.

The host harness limits the game's allocations to 3.7 MB and fills new
memory with a pattern, as the board would have it; the most it reported
in use while playing level 1 was 2.8 MB.

## Video

The game draws into its 8 bpp screen (`drawSurface`, 640 x 480; the 3D
view goes through `offscreenDrawSurface`). Without the span engine,
`SDLDraw()` copies it into one of three display buffers, cleans it from the
data cache and has the vertical-blank interrupt show it, its rows spread
over the 720 lines (480 on 1 or 2 each; 200 on 3 or 4 at 320 x 200, its
columns doubled once more in the copy), loading the palette (through
Shockolate's gamma table, as RGB565) at the same time. Frames the engine
drew in show the view's rows in RGB565 and the rest from the screen
itself (The span engine).

`../core/tools/m9bctl.py shot OUT.png` takes a screenshot over the UART and
`m9bctl.py keys ...` types keys.

## Sound

The audio interrupt mixes the next 256 frames of the 48 kHz output at
24 kHz (each result held for 2 frames), in stereo: the speech stream
(8-bit 22 kHz, cutscenes and audio logs), then the 12 effect channels:
the game's VOC samples (8-bit, 11 or 22 kHz), decoded into a signed copy
when played, with its volume and pan (as SDL_mixer's panning).

## Music

The game's music is adaptive: each theme (`SOUND/SBLASTER/THM*.XMI`,
`TITLOOP`, `ENDLOOP`, ...) holds up to 54 short XMI sequences, and the music
AI (`musicai.c`, `mlimbs.c`) starts them one at a time on up to 8 threads,
the main piece on thread 0 and shorter layers repeated on the others,
polling `IsPlaying()` to queue the next. Shockolate plays them through an
OPL emulator or FluidSynth, neither of which fits this CPU, so
`tools/make_music.sh SSHOCKDIR OUTDIR [--upload]` renders every sequence on
its own with libADLMIDI (`../core/tools/midirender`: Nuked OPL3, the
game's own AdLib timbres `SOUND/CIT.AD` over the "AIL (System Shock)" bank,
Miles' volume model) to `<THEME>_<N>.WAV`, 24 kHz 16-bit mono with one gain
per theme, so layers keep their balance (the theme's loudness to -18 LUFS,
its loudest peak at most -1 dBFS). The silent sequences (each theme's
control sequence 0, placeholders) are left out. 227 files, 35 minutes,
95 MB; `--upload` puts them in `SSHOCK/MUSIC/`.

`MacTuneLoadTheme` (`port/m9b_sound.c`) reads the theme's music AI tables
(which piece follows which, the layers, the transitions) from
`res/sound/<theme>.bin`, the DOS version's `SOUND/THMn.BIN`, as upstream's
`Xmi.c` does; without them the AI keeps choosing the first piece.
`port/m9b_sound.c` implements `Xmi.c`'s interface over them: `StartTrack`
opens the sequence's file on its thread, the main loop (`m9b_sound_poll`)
keeps each playing thread's 16 KB ring (0.34 s, 128 KB in all) filled, and
the audio interrupt adds the threads into the mix; a thread whose file
ends (or has none) stops playing, so the AI queues the next piece as it
would. `curr_vol_lev` (0..100, ducked by the audio logs) is the volume, 100
playing the render as it is. Checked in the host harness: the main menu
plays `TITLOOP_0.WAV` at the music volume (75); in a new game the main
pieces change (5, 6, 2, 1, 3, 4 ...) with a layer over each, two files open
at a time.

## The host test harness

`make host` builds `build-host/sshock-host`, the game and port code for
32-bit x86 Linux (gcc-multilib): the SoC's I/O registers are plain memory
mapped at their own addresses, a thread keeps the clock counter running,
and the harness plays the hardware's part from the game loop (vertical
blanks, audio halves, written to `OUT/audio.wav`; USB keyboard and mouse
reports from a script; screenshots). It also limits the heap to the
board's and reports the open files (the board has 40).

```
tools/host/make_datadir.sh ~/.local/share/Steam/steamapps/common/SS1/SSHOCK /tmp/ss-data
build-host/sshock-host /tmp/ss-data /tmp/ss-out SCRIPT
```

with SCRIPT lines like `12000 key 0x29` (time in ms, HID usage: Esc),
`14000 move 165 107`, `15200 click`, `20000 shot name`, `60000 email 1`
(opens email 1, as the email panel would), `90000 quit` (see
`tools/host/host.c`). On the board, Ctrl-E on the UART opens email 1 too.
(The host build doesn't link at the moment: `m20_screenshot`, from the
core's runtime, is missing there.)

## Changes to Shockolate

Marked `M9B`:
- `RES/Source/`: the resource table's size (`res.c`), compound resources
  read item by item (`refacc.c`, `resload.c`), the files' names kept for
  reading in place (`resfile.c`, `res.h`), no case-insensitive directory
  search (`caseless.c`: FatFs is case-insensitive).
- `AFILE/Source/`: movies read in place and their sound a block at a time
  (`movie.c`, `amov.c`, `afile.c`, `afile.h`).
- `GameSrc/cutsloop.c`, `audiolog.c`: the low resolution cutscenes, the
  sound fed as it plays, the movies freed, a cutscene that can't be opened
  going back to the menu (upstream went on without a movie and crashed).
- `GameSrc/tools.c`: a palette read from the file (compound resources' data
  stay there); a pointer that could be used uninitialized.
- `2D/Source/RSD/RSDUnpack.c`: the texture unpacker without unaligned
  word accesses (Building).
- `FIX/Source/fix.c`: 64-by-32-bit divisions through the hardware divider
  when the quotient fits (`-DM9B_DIV_STATS` counts the paths).
- `FIX/Source/fix_sqrt.c`, `3D/Source/vector.c`: integer square roots (the
  float ones are slow library code without an FPU; `g3_vec_mag` was
  128-bit `long double`).
- `MacSrc/Prefs.c`: 320 x 200 by default (Shockolate's is 640 x 480).
- `UI/Source/event.c`: `uiInit` returned an uninitialized value; a
  pending click expires at the input's time, not the current one (double
  clicks at a low frame rate, see Input).
- `FIXPP/Source/fixpp.h`, `EDMS`: no `<iostream>`; `int` constructors and
  operators for `Fixpoint` (with `int32_t` being `long`, plain ints were
  ambiguous).
- `GameSrc/musicai.c`, `MacSrc/Modding.c`: no OPL synthesizer, no
  directory listing.

Unmarked, as they're right everywhere: declarations that said `int` where
the definitions said `int32_t` (or the other way round), which C++ mangles
differently and C calls conflicting on riscv32 (`afile.c`, `tfdirect.c`,
EDMS); `fix_pow.c` dropped (its function is also in `fix.c`; upstream's
static libraries hid the duplicate). Removed from the tree: the SDL
platform files `port/` replaces, the OpenGL renderer, the MIDI
synthesizers (`adlmidi`, `MusicSrc`), the `SND` library (upstream didn't
link it either), CMake files.

## Next steps

- On the board: the heap over a long game; misaligned accesses (2D and 3D
  structures are `#pragma pack(2)`).
- Music: check the in-game themes and their layers on the board.
- Speed: each engine polygon's setup, the composing, the on-line help's
  object scan (`olh_scan_objs`); the engine's texel misses on 128 x 128
  textures (`make B=build-prof`).
- The occasional `ResLock: Could not load 112` at 640 x 480 (memory?).
- The host build's link.
