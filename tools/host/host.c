// A host (x86, 32-bit Linux) test harness for the port: runs the same game
// and port code (port/*.c but m9b_posix.c) without the board, with the span
// engine (M20_ENGINE, the default `make host`) done by its C model
// (../../core/firmware/span_emu.c), so the picture is the board's.
//
// Memory: the program is linked at 0x800000 (-Ttext-segment) and the
// game's heap is an arena in its .bss the board's size (M20_HEAP: from
// __heap_start in the board build's ELF, up to the 128 KB stack under
// 0x1000000; the Makefile), so everything the engine reads or writes (the
// display buffers, textures, lightmaps) is in the board's SDRAM window
// 0x800000..0xFFFFFF and the port's SDRAM_OFF(p) = p - 0x800000 holds (it
// is checked: m20_host_sdram_off). The game's malloc and friends (wrapped)
// come from the arena, first fit with 8-byte headers like picolibc's, and
// new memory is filled with 0xA5 (the board's heap holds whatever was in
// SDRAM before); the most in use is reported, and M9B_HEAP_KB in the
// environment lowers the cap.
//
// The SoC's I/O window (0x400000..0x7FFFFF) is mapped as plain memory at its
// own address, so the port's register accesses land in it. CLOCK_CNT is
// m20_host_clock() (m9b_hw.h): virtual time by default, 1 us a read and at
// least $SSHOCK_FRAME_MS (50) a frame, so runs are reproducible and the
// builds with and without the engine play the same game; 0: real time. The
// wrapped pump_events() (the game calls it every loop) plays the hardware's
// other part:
//  - every 16.7 ms a vertical blank (vblank_flip(), as the interrupt would:
//    it writes the line table and palette),
//  - every 5.33 ms an audio half (sound_irq()), appended to OUT/audio.wav
//    (48 kHz 16-bit stereo, what the HDMI audio would get),
//  - the script's input, through the USB registers (keyboard on port 0,
//    mouse on port 1, as the hardware reports them),
//  - screenshots: the picture the video hardware makes from the line table
//    and palette (the frame shown: with the engine, its RGB565 view rows and
//    the 8 bpp interface rows) to OUT/NAME.ppm.
//
// Files: the game's paths are relative to the card's SSHOCK/ (as
// m9b_posix.c makes them on the board). Reads look in the save directory
// first, then in the data directory, case-insensitively (FatFs is; the
// card's names are upper case); whatever the game writes (prefs.txt,
// keybinds.txt, CurrentGame.dat, the saves) goes to the save directory,
// never into the data.
//
// Window (M20_WINDOW=1, when built with SDL2: window.c): the picture the
// video hardware makes (as the screenshots, 1280 x 720) shown at each
// vertical blank; SDL's keys and mouse go into the same USB registers as
// the script's (keyboard: the HID usages and modifier byte; mouse: the
// pointer put where the host's is over the picture, or SDL's deltas while
// the game's mouselook is on); F12 a screenshot (OUT/f12-NNN.ppm); real
// time unless $SSHOCK_FRAME_MS is set; the SCRIPT is optional.
//
// usage: sshock-host [DATADIR] OUTDIR SCRIPT
//        M20_WINDOW=1 sshock-host [DATADIR] OUTDIR [SCRIPT]
//   DATADIR: the card's root (holding SSHOCK/, as ../../sdcard/make_sdcard.sh
//   builds it in OUT/card) or its SSHOCK/ itself (holding RES/DATA/...), or
//   a directory tools/host/make_datadir.sh made; without it, $SSHOCK_DATA.
//   OUTDIR: screenshots, audio.wav; the save directory is OUTDIR/SSHOCK
//   unless $SSHOCK_SAVE names another.
//   SCRIPT lines: "MS key HID" (press and release, 100 ms), "MS down HID",
//   "MS up HID", "MS move DX DY", "MS click [left|right]", "MS shot NAME"
//   (the picture shown), "MS screen NAME" (the game's 8 bpp screen as drawn,
//   with the engine's holes), "MS email N" (opens email N), "MS res" (the
//   resource cache's memory by file, locked and not), "MS sfx N [M]" (plays
//   sound effects N to M, one a frame, as the game would), "MS quit"; MS is
//   the (virtual) time since start in milliseconds, HID a USB HID usage (hex with 0x,
//   e.g. 0x28 Enter, 0x29 Esc, 0x52 Up). '#' comments.
//   With the window, tests of its input path: "MS wkey HID", "MS wdown HID",
//   "MS wup HID", "MS wmove X Y" (X, Y in the 1280 x 720 picture),
//   "MS wclick [left|right]", "MS wquit" (the window closed): SDL events pushed into SDL's queue, which
//   then go the way the window's own events do.
//   $SSHOCK_SHOT_FULL=1: screenshots 1280 x 720 (else the game's resolution).
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "SDL.h"
#include "m9b_map.h"
#include "res.h"
#include "clock.h"   // CLOCK_HZ: ../../core/firmware
#ifdef M20_ENGINE
#include "span_emu.h"
#endif

#define IO_BASE   M9B_IO_BASE
#define IO_SIZE   0x00400000u
#define REG(a)    (*(volatile uint32_t*)(uintptr_t)(a))
#define CLOCK_CNT REG((M9B_TIMING + 0xCu))
#define VRAM_PALETTE ((volatile uint32_t*)(uintptr_t)(M9B_VRAM + 0x800u))
#define VRAM_TABLE   ((volatile uint32_t*)(uintptr_t)(M9B_VRAM + 0xA00u))
#define VIDEO_HWIN   REG((M9B_CTRL + 0x18u))
#define AUDIO_CTRL REG((M9B_CTRL + 0xCu))
#define AUDIO_PTR(h) REG((M9B_CTRL + 0x10u) + 4u * (h))
#define USB(p, o) REG(M9B_USB + 0x20u * (p) + (o))
#define PSRAM_BASE M9B_PSRAM_BASE
#define SDRAM_BASE 0x800000u
#define SDRAM_END  0x1000000u

// The board's heap (the Makefile: 0xFE0000 - __heap_start of build/sshock.elf)
#ifndef M20_HEAP
#define M20_HEAP 0x4EE280u
#endif

extern void __real_pump_events(void);
extern void vblank_flip(void);
extern void sound_irq(uint32_t pending);
extern SDL_Surface *drawSurface;
extern int MouseX, MouseY;                 // m9b_events.c: the game's pointer
extern int32_t gScreenWide, gScreenHigh;   // m9b_shock.c
extern char _end[];

// (the harness's own files and memory: the C library's)
extern FILE *__real_fopen(const char *path, const char *mode);
extern void *__real_malloc(size_t n);
extern void *__real_realloc(void *p, size_t n);
extern void __real_free(void *p);

static uint64_t t_start;
static const char *out_dir;
static char *data_dir, *save_dir;
static FILE *wav;
static uint32_t wav_bytes;
static int shot_full;
static int window;   // M20_WINDOW=1: window.c

#ifdef M20_SDL
extern int win_open(const char *title, int with_audio);
extern void win_close(void);
extern void win_present(const uint32_t *pic);
extern void win_audio(const void *buf, int bytes);
extern void win_set_relative(int on, int x, int y);
extern void win_poll(void);
extern void win_push_key(int scancode, int down);
extern void win_push_motion(int x, int y);
extern void win_push_button(int b, int down);
extern void win_push_quit(void);
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

// Time: CLOCK_CNT (CLOCK_HZ, as the board's) is m20_host_clock(). Virtual
// by default (SSHOCK_FRAME_MS, 50): it goes on 1 us at each read, and each
// frame (SDLDraw, m9b_frames) takes at least SSHOCK_FRAME_MS, so runs are
// reproducible and builds with and without the engine see the same game;
// SSHOCK_FRAME_MS=0: real time.
static uint32_t frame_ms = 50;
static uint64_t vclock;   // CLOCK_CNT, 64 bits

uint32_t m20_host_clock(void) {
    if (frame_ms) vclock += CLOCK_PER_US;
    else vclock = (now_ns() - t_start) * (CLOCK_HZ / 1000000u) / 1000u;
    CLOCK_CNT = (uint32_t)vclock;   // (the register too: m20_engine.c reads it for its counters)
    return (uint32_t)vclock;
}

static uint64_t host_us(void) {   // the time CLOCK_CNT shows, without moving it on
    return frame_ms ? vclock / CLOCK_PER_US : (now_ns() - t_start) / 1000u;
}

static uint32_t now_ms(void) { return (uint32_t)(host_us() / 1000u); }

__attribute__((constructor)) static void map_io(void) {
    void *p = mmap((void *)(uintptr_t)IO_BASE, IO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)IO_BASE) {
        perror("mapping the I/O window");
        exit(1);
    }
    t_start = now_ns();
}

uint32_t m20_host_sdram_off(const void *p) {
    uintptr_t a = (uintptr_t)p;
    if (a < SDRAM_BASE || a >= SDRAM_END) {
        fprintf(stderr, "host: SDRAM_OFF(%p): outside the SDRAM window 0x800000..0xFFFFFF\n", p);
        abort();
    }
    return (uint32_t)(a - SDRAM_BASE);
}

// -------------------------------------------------------------------------
// The heap: first fit over an arena the board's size, 8-byte headers (size
// with bit 0 = in use, the previous block's size), coalescing.
// aligned_alloc puts a shim (an even magic, the block's pointer) before its
// pointer. (As ../../descent/tools/host/host.c's.)
// -------------------------------------------------------------------------
typedef struct { uint32_t size, prev; } blk_t;
static uint8_t arena[M20_HEAP] __attribute__((aligned(4096)));
static blk_t *first_free;   // no free block below it
static size_t heap_used, heap_peak, heap_cap = M20_HEAP;
static unsigned heap_refused;
static int probing;
#define ALIGN_MAGIC 0xA11C0DE0u
#define ARENA_END   (arena + (M20_HEAP & ~7u))

static blk_t *blk_next(blk_t *b) {
    uint8_t *n = (uint8_t *)b + (b->size & ~7u);
    return n < ARENA_END ? (blk_t *)n : NULL;
}

static int in_arena(const void *p) { return (const uint8_t *)p >= arena && (const uint8_t *)p < ARENA_END; }

static blk_t *block_of(void *p) {
    uint32_t *w = (uint32_t *)p - 2;
    if (w[0] == ALIGN_MAGIC) p = (void *)(uintptr_t)w[1];
    return (blk_t *)p - 1;
}

// The bytes usable at p (the game's pointer), as malloc_usable_size
static size_t usable(void *p) {
    if (!p) return 0;
    if (!in_arena(p)) return malloc_usable_size(p);
    blk_t *b = block_of(p);
    return (size_t)((uint8_t *)b + (b->size & ~7u) - (uint8_t *)p);
}

static size_t largest_free(void) {
    size_t m = 0;
    for (blk_t *b = first_free; b; b = blk_next(b))
        if (!(b->size & 1u) && b->size - sizeof(blk_t) > m) m = b->size - sizeof(blk_t);
    return m;
}

static void *arena_alloc(size_t size) {
    if (!first_free) {
        blk_t *b = (blk_t *)arena;
        b->size = M20_HEAP & ~7u;
        b->prev = 0;
        first_free = b;
    }
    if (size > M20_HEAP) goto refuse;
    uint32_t need = (uint32_t)((size + sizeof(blk_t) + 7u) & ~7u);
    if (heap_used + need > heap_cap) goto refuse;
    for (blk_t *b = first_free; b; b = blk_next(b)) {
        if ((b->size & 1u) || b->size < need) continue;
        if (b->size - need >= 64u) {
            blk_t *rest = (blk_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->prev = need;
            blk_t *after = blk_next(rest);
            if (after) after->prev = rest->size;
            b->size = need;
        }
        b->size |= 1u;
        heap_used += b->size & ~7u;
        if (heap_used > heap_peak && !probing) heap_peak = heap_used;   // (not the probe)
        if (b == first_free)
            while (first_free && (first_free->size & 1u)) first_free = blk_next(first_free);
        void *p = b + 1;
        memset(p, 0xA5, (b->size & ~7u) - sizeof(blk_t));   // (the board's heap isn't cleared)
        return p;
    }
refuse:
    if (probing) return NULL;   // (slorkatron_memory_check's probe at the start: fills the heap)
    heap_refused++;
    static size_t last;   // (the pager asks again for the same size after each resource it drops: once)
    if (size != last)
        printf("[host %u ms] malloc of %zu bytes refused (%zu KB in use of %zu KB, the largest free block %zu KB)\n",
               now_ms(), size, heap_used / 1024, heap_cap / 1024, largest_free() / 1024);
    last = size;
    return NULL;
}

// The live allocations of 8 KB or more and where they were made, for "res".
#define BIG_MIN 8192u
#define BIG_N   4096
static struct { void *p; size_t n; void *caller; } big[BIG_N];
static void big_add(void *p, size_t n, void *caller) {
    if (!p || n < BIG_MIN) return;
    for (int i = 0; i < BIG_N; i++)
        if (!big[i].p) { big[i].p = p; big[i].n = n; big[i].caller = caller; return; }
}
static void big_del(void *p) {
    if (!p) return;
    for (int i = 0; i < BIG_N; i++)
        if (big[i].p == p) { big[i].p = NULL; return; }
}

void __wrap_free(void *p) {
    if (!p) return;
    if (!in_arena(p)) { __real_free(p); return; }   // (the C library's own)
    big_del(p);
    blk_t *b = block_of(p);
    if (!(b->size & 1u)) { fprintf(stderr, "host: free(%p) of a free block\n", p); abort(); }
    b->size &= ~1u;
    heap_used -= b->size;
    blk_t *n = blk_next(b);
    if (n && !(n->size & 1u)) {
        b->size += n->size;
        blk_t *nn = blk_next(b);
        if (nn) nn->prev = b->size;
    }
    if (b->prev) {
        blk_t *pb = (blk_t *)((uint8_t *)b - b->prev);
        if (!(pb->size & 1u)) {
            pb->size += b->size;
            blk_t *nn = blk_next(pb);
            if (nn) nn->prev = pb->size;
            b = pb;
        }
    }
    if (b < first_free) first_free = b;
}

void *__wrap_malloc(size_t n) {
    extern int slorkatron_memory_check();
    void *caller = __builtin_return_address(0);
    probing = (uintptr_t)caller - (uintptr_t)slorkatron_memory_check < 0x200u;
    void *p = arena_alloc(n);
    probing = 0;
    big_add(p, n, __builtin_return_address(0));
    return p;
}

void *__wrap_calloc(size_t n, size_t m) {
    void *p = arena_alloc(n * m);
    if (p) memset(p, 0, n * m);
    big_add(p, n * m, __builtin_return_address(0));
    return p;
}

void *__wrap_aligned_alloc(size_t a, size_t n) {
    uint8_t *p;
    if (a <= 8) p = arena_alloc(n);
    else {
        uint8_t *raw = arena_alloc(n + a + 8);
        p = raw;
        if (raw) {
            p = (uint8_t *)(((uintptr_t)raw + 8 + a - 1) & ~(uintptr_t)(a - 1));
            ((uint32_t *)p)[-2] = ALIGN_MAGIC;
            ((uint32_t *)p)[-1] = (uint32_t)(uintptr_t)raw;
        }
    }
    big_add(p, n, __builtin_return_address(0));
    return p;
}

void *__wrap_realloc(void *p, size_t n) {
    if (!p) {
        void *q = arena_alloc(n);
        big_add(q, n, __builtin_return_address(0));
        return q;
    }
    if (!in_arena(p)) { fprintf(stderr, "host: realloc of a pointer not from the arena\n"); abort(); }
    size_t have = usable(p);
    if (have >= n) return p;
    void *q = arena_alloc(n);
    if (q) {
        memcpy(q, p, have);
        __wrap_free(p);
        big_add(q, n, __builtin_return_address(0));
    }
    return q;
}

char *__wrap_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// -------------------------------------------------------------------------
// Files: the data directory (read only, case-insensitive) and the save
// directory (what the game writes)
// -------------------------------------------------------------------------
// dir/rel with each part matched case-insensitively (an exact match first)
// into out; 1 if it exists
static int find_ci(const char *dir, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, rel);
    struct stat st;
    if (stat(out, &st) == 0) return 1;
    size_t len = (size_t)snprintf(out, n, "%s", dir);
    const char *p = rel;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t pl = e ? (size_t)(e - p) : strlen(p);
        if (pl == 0 || (pl == 1 && p[0] == '.')) { p += pl + (e != NULL); continue; }
        char part[256];
        if (pl >= sizeof(part)) return 0;
        memcpy(part, p, pl);
        part[pl] = 0;
        DIR *d = opendir(out);
        if (!d) return 0;
        struct dirent *de;
        int found = 0;
        while ((de = readdir(d)))
            if (!strcasecmp(de->d_name, part)) {
                len += (size_t)snprintf(out + len, len < n ? n - len : 0, "/%s", de->d_name);
                found = 1;
                break;
            }
        closedir(d);
        if (!found || len >= n) return 0;
        p += pl + (e != NULL);
    }
    return 1;
}

static void mkdirs_for(const char *path) {   // the directories above path
    char b[1024];
    snprintf(b, sizeof(b), "%s", path);
    for (char *s = b + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(b, 0777); *s = '/'; }
}

static int copy_to(const char *from, const char *to) {
    FILE *a = __real_fopen(from, "rb"), *b = a ? __real_fopen(to, "wb") : NULL;
    if (!b) { if (a) fclose(a); return -1; }
    char buf[65536];
    size_t k;
    while ((k = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, k, b);
    fclose(a);
    fclose(b);
    return 0;
}

// The host path for the game's path: writing, in the save directory (a
// file only in the data copied there first when it is read too); reading,
// the save directory's, else the data's
static const char *host_path(const char *path, int write, int read_too, char *out, size_t n) {
    if (path[0] == '/') return path;
    while (path[0] == '.' && path[1] == '/') path += 2;
    char data[1024];
    if (find_ci(save_dir, path, out, n)) return out;
    int in_data = find_ci(data_dir, path, data, sizeof(data));
    if (!write) {
        if (in_data) snprintf(out, n, "%s", data);
        else snprintf(out, n, "%s/%s", save_dir, path);   // (not there: the error from the save directory)
        return out;
    }
    snprintf(out, n, "%s/%s", save_dir, path);
    mkdirs_for(out);
    if (read_too && in_data) copy_to(data, out);
    return out;
}

extern int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    char b[1024];
    int acc = flags & O_ACCMODE, write = acc != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND));
    return __real_open(host_path(path, write, acc == O_RDWR && !(flags & O_TRUNC), b, sizeof(b)), flags, mode);
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    char b[1024];
    int write = strpbrk(mode, "wa+") != NULL, read_too = mode[0] == 'a' || (mode[0] == 'r' && strchr(mode, '+'));
    return __real_fopen(host_path(path, write, read_too, b, sizeof(b)), mode);
}

extern int __real_unlink(const char *path);
int __wrap_unlink(const char *path) {   // (the save directory's only)
    char b[1024];
    if (path[0] != '/' && !find_ci(save_dir, path, b, sizeof(b))) { errno = ENOENT; return -1; }
    return __real_unlink(path[0] == '/' ? path : b);
}

int __wrap_remove(const char *path) { return __wrap_unlink(path); }

extern int __real_rename(const char *a, const char *b);
int __wrap_rename(const char *a, const char *b) {
    char ba[1024], bb[1024];
    if (a[0] != '/' && !find_ci(save_dir, a, ba, sizeof(ba))) { errno = ENOENT; return -1; }
    const char *pb = b[0] == '/' ? b : find_ci(save_dir, b, bb, sizeof(bb)) ? bb : (snprintf(bb, sizeof(bb), "%s/%s", save_dir, b), bb);
    return __real_rename(a[0] == '/' ? a : ba, pb);
}

extern int __real_access(const char *path, int mode);
int __wrap_access(const char *path, int mode) {
    char b[1024];
    return __real_access(host_path(path, 0, 0, b, sizeof(b)), mode);
}

// -------------------------------------------------------------------------
// Script
// -------------------------------------------------------------------------
enum { KEY, DOWN, UP, MOVE, CLICK, SHOT, SCREEN, EMAIL, RES, SFX, QUIT, WKEY, WDOWN, WUP, WMOVE, WCLICK, WQUIT };
static struct step { uint32_t ms; int op, a, b; char name[64]; } *steps;
static int nsteps, next_step;

static void load_script(const char *path) {
    FILE *f = __real_fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char op[16] = "", a[64] = "", b[64] = "";
        unsigned ms;
        if (sscanf(line, "%u %15s %63s %63s", &ms, op, a, b) < 2) continue;
        steps = __real_realloc(steps, sizeof(*steps) * (nsteps + 2));
        struct step *s = &steps[nsteps++];
        memset(s, 0, sizeof(*s));
        s->ms = ms;
        if (!strcmp(op, "key")) { s->op = KEY; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "down")) { s->op = DOWN; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "up")) { s->op = UP; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "move")) { s->op = MOVE; s->a = atoi(a); s->b = atoi(b); }
        else if (!strcmp(op, "click")) { s->op = CLICK; s->a = !strcmp(a, "right") ? 2 : 1; }
        else if (!strcmp(op, "shot")) { s->op = SHOT; snprintf(s->name, sizeof(s->name), "%s", a); }
        else if (!strcmp(op, "screen")) { s->op = SCREEN; snprintf(s->name, sizeof(s->name), "%s", a); }
        else if (!strcmp(op, "email")) { s->op = EMAIL; s->a = atoi(a); }
        else if (!strcmp(op, "res")) s->op = RES;
        else if (!strcmp(op, "sfx")) { s->op = SFX; s->a = atoi(a); s->b = *b ? atoi(b) : s->a; }
        else if (!strcmp(op, "quit")) s->op = QUIT;
        else if (!strcmp(op, "wkey")) { s->op = WKEY; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "wdown")) { s->op = WDOWN; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "wup")) { s->op = WUP; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "wmove")) { s->op = WMOVE; s->a = atoi(a); s->b = atoi(b); }
        else if (!strcmp(op, "wclick")) { s->op = WCLICK; s->a = !strcmp(a, "right") ? 2 : 1; }
        else if (!strcmp(op, "wquit")) s->op = WQUIT;
        else { fprintf(stderr, "script: unknown '%s'\n", op); exit(1); }
        if (s->op >= WKEY && !window) { fprintf(stderr, "script: '%s' needs the window (M20_WINDOW=1)\n", op); exit(1); }
        if (s->op == KEY || s->op == CLICK || s->op == WKEY || s->op == WCLICK) {   // the release, 100 ms later
            int k = s->op == KEY || s->op == WKEY, w = s->op >= WKEY;
            struct step *r = &steps[nsteps++];
            *r = *s;
            r->ms = s->ms + 100u;
            r->op = k ? (w ? WUP : UP) : (w ? WCLICK : CLICK);
            r->a = k ? s->a : -s->a;
            s->op = k ? (w ? WDOWN : DOWN) : (w ? WCLICK : CLICK);
        }
    }
    fclose(f);
    for (int i = 1; i < nsteps; i++)   // stable sort by time
        for (int j = i; j > 0 && steps[j - 1].ms > steps[j].ms; j--) {
            struct step t = steps[j]; steps[j] = steps[j - 1]; steps[j - 1] = t;
        }
}

// Keyboard: up to 4 keys held, as the USB registers report them, and the
// modifier byte (the window's Ctrl, Shift, Alt, GUI; the script's modifiers
// are held as keys, 0xE0..0xE7, which the port takes either way).
static uint8_t held[4], held_mods;

static void report_keys(void) {
    USB(0, 0x04) = held_mods | (uint32_t)held[0] << 8 | (uint32_t)held[1] << 16 | (uint32_t)held[2] << 24;
    USB(0, 0x08) = held[3];
    USB(0, 0x00) = 1u | ((USB(0, 0x00) >> 8) + 1u & 0xFFu) << 8;   // keyboard, next report
}

// -------------------------------------------------------------------------
// Screenshots: the picture as the video hardware makes it from the line
// table and palette (the frame shown, as the last vertical blank set it
// up), 1280 x 720, or at the game's resolution (each screen pixel's first
// picture pixel)
// -------------------------------------------------------------------------
static uint32_t rgb565_to_888(uint32_t c) {
    uint32_t r = c >> 11 & 31u, g = c >> 5 & 63u, b = c & 31u;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static void picture(uint32_t *pic) {   // 1280 x 720
    uint32_t hw = VIDEO_HWIN, l = hw & 0xFFFFu, r = hw >> 16;
    for (uint32_t y = 0; y < 720u; y++) {
        uint32_t e = VRAM_TABLE[y], type = e >> 22 & 3u, dbl = e >> 28 & 1u;
        uint32_t off = (e & 0x7FFFu) << 8 | (e >> 15 & 0x7Fu) * 2u;
        const uint8_t *src = (const uint8_t *)(uintptr_t)(SDRAM_BASE + off);
        for (uint32_t x = 0; x < 1280u; x++) {
            uint32_t c = 0;
            if (type && x >= l && x < r) {
                uint32_t i = dbl ? (x - l) / 2u : x - l;
                if (type == 2u) {
                    uint32_t k = src[i];
                    c = rgb565_to_888(VRAM_PALETTE[k / 2u] >> (16u * (k & 1u)) & 0xFFFFu);
                } else if (type == 1u)
                    c = rgb565_to_888(((const uint16_t *)src)[i]);
            }
            pic[y * 1280u + x] = c;
        }
    }
}

static void write_ppm(const char *name, int w, int h, const uint32_t *px, int stride) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    FILE *f = __real_fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * stride + x];
            uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    printf("[host %u ms] shot %s\n", now_ms(), path);
}

static void screenshot(const char *name) {
    static uint32_t *pic, *small;
    if (!pic) pic = __real_malloc(4u * 1280u * 720u), small = __real_malloc(4u * 1280u * 720u);
    picture(pic);
    if (shot_full || !drawSurface) { write_ppm(name, 1280, 720, pic, 1280); return; }
    uint32_t hw = VIDEO_HWIN, l = hw & 0xFFFFu, r = hw >> 16;
    int w = drawSurface->w, h = drawSurface->h;
    if (r <= l || w <= 0 || h <= 0 || h > 720) { write_ppm(name, 1280, 720, pic, 1280); return; }
    for (int y = 0; y < h; y++) {
        uint32_t line = ((uint32_t)y * 720u + (uint32_t)h - 1u) / (uint32_t)h;   // the first line showing row y
        for (int x = 0; x < w; x++) small[y * w + x] = pic[line * 1280u + l + (uint32_t)x * (r - l) / (uint32_t)w];
    }
    write_ppm(name, w, h, small, w);
}

static void screen_shot(const char *name) {   // the game's 8 bpp screen, through the palette loaded
    static uint32_t *pic;
    if (!drawSurface) return;
    int w = drawSurface->w, h = drawSurface->h;
    if (!pic) pic = __real_malloc(4u * 640u * 720u);
    if (w > 640 || h > 720) return;
    const uint8_t *px = drawSurface->pixels;
    for (int i = 0; i < w * h; i++)
        pic[i] = rgb565_to_888(VRAM_PALETTE[px[i] / 2u] >> (16u * (px[i] & 1u)) & 0xFFFFu);
    write_ppm(name, w, h, pic, w);
}

// Files open now (the board has 40: ../../core/runtime/syscalls.c's MAX_FILES),
// less the window's (SDL's descriptors, as it opened)
static int fd_window;
static int open_files(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n - 4 - fd_window;   // stdin, stdout, stderr and the directory itself
}

static int files_peak;

static void report_memory(void) {
    printf("[host %u ms] heap in use %zu KB, most %zu KB, %u refused (cap %zu KB, the largest free block %zu KB); %d files open, most %d\n",
           now_ms(), heap_used / 1024, heap_peak / 1024, heap_refused, heap_cap / 1024, largest_free() / 1024, open_files(), files_peak);
}

static void report(void) {   // at exit
    report_memory();
#ifdef M20_ENGINE
    printf("[host] span_emu: %u spans, %u pixels (%u processed), %u EDGE, %u lightmaps, %u zero spans, %u z layout, %u z stale\n",
           span_emu_stats.spans, span_emu_stats.pixels, span_emu_stats.pixels_in, span_emu_stats.edges,
           span_emu_stats.lightmaps, span_emu_stats.zero_spans, span_emu_stats.z_layout, span_emu_stats.z_stale);
#endif
}

// The resource cache: the memory each file's loaded resources take
// (compound ones with their ref table and items), locked and not.
static void report_res(void) {
    extern const ResourceFormat RefTableFormat;
    size_t locked[MAX_RESFILENUM + 1] = {0}, unlocked[MAX_RESFILENUM + 1] = {0}, all = 0;
    int nl[MAX_RESFILENUM + 1] = {0}, nu[MAX_RESFILENUM + 1] = {0};
    for (Id id = ID_MIN; id <= resDescMax; id++) {
        ResDesc *prd = RESDESC(id);
        if (!prd->ptr || prd->filenum > MAX_RESFILENUM) continue;
        size_t n = usable(prd->ptr);
        if (prd->format == &RefTableFormat) {
            RefTable *prt = prd->ptr;
            if (prt->raw_data) n += usable(prt->raw_data);
            for (int i = 0; i < prt->numRefs; i++)
                if (prt->entries[i].decoded_data) n += usable(prt->entries[i].decoded_data);
        }
        if (prd->lock) { locked[prd->filenum] += n; nl[prd->filenum]++; }
        else { unlocked[prd->filenum] += n; nu[prd->filenum]++; }
        all += n;
    }
    printf("[host %u ms] resource cache %zu KB (heap in use %zu KB):\n", now_ms(), all / 1024, heap_used / 1024);
    for (int f = 0; f <= MAX_RESFILENUM; f++)
        if (nl[f] || nu[f])
            printf("  %-24s locked %4d %6zu KB, unlocked %4d %6zu KB\n", ResFileName(f) ? ResFileName(f) : "?", nl[f],
                   locked[f] / 1024, nu[f], unlocked[f] / 1024);
    // The allocations of 8 KB or more by caller (addr2line -f -e sshock-host ADDR)
    size_t big_all = 0;
    for (int i = 0; i < BIG_N; i++) {
        if (!big[i].p || big[i].n == 0) continue;
        void *c = big[i].caller;
        size_t sum = 0;
        int cnt = 0;
        for (int j = i; j < BIG_N; j++)
            if (big[j].p && big[j].n && big[j].caller == c) { sum += big[j].n; cnt++; if (j > i) big[j].n = 0; }
        big_all += sum;
        printf("  big: %p %4d blocks %6zu KB\n", c, cnt, sum / 1024);
    }
    for (int i = 0; i < BIG_N; i++)   // the sizes back (zeroed above while summing)
        if (big[i].p && big[i].n == 0) big[i].n = usable(big[i].p);
    printf("  big: %zu KB in all\n", big_all / 1024);
}

static void finish_wav(void) {   // the sizes in the header
    if (!wav) return;
    fseek(wav, 4, SEEK_SET);
    uint32_t riff = 36u + wav_bytes;
    fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET);
    fwrite(&wav_bytes, 4, 1, wav);
    fclose(wav);
    wav = NULL;
}

static void host_quit(const char *why) {
    finish_wav();
    printf("[host %u ms] %s\n", now_ms(), why);
#ifdef M20_SDL
    if (window) win_close();
    fd_window = 0;   // (SDL's closed)
#endif
    exit(0);   // (atexit: report)
}

static int pushed;   // the script pushed SDL events (WKEY...): poll the window now

static void run_script(void) {
    uint32_t t = now_ms();
    while (next_step < nsteps && steps[next_step].ms <= t) {
        struct step *s = &steps[next_step++];
        switch (s->op) {
        case DOWN:
            for (int i = 0; i < 4; i++) if (!held[i]) { held[i] = (uint8_t)s->a; break; }
            report_keys();
            break;
        case UP:
            for (int i = 0; i < 4; i++) if (held[i] == s->a) held[i] = 0;
            report_keys();
            break;
        case MOVE: {
            uint32_t pos = USB(1, 0x14);
            uint16_t x = (uint16_t)(pos + (uint32_t)s->a), y = (uint16_t)((pos >> 16) + (uint32_t)s->b);
            USB(1, 0x14) = (uint32_t)y << 16 | x;
            break;
        }
        case CLICK:
            if (s->a > 0) USB(1, 0x0C) |= s->a == 2 ? 2u : 1u;
            else USB(1, 0x0C) &= ~(s->a == -2 ? 2u : 1u);
            break;
        case SHOT:
            screenshot(s->name);
            break;
        case SCREEN:
            screen_shot(s->name);
            break;
        case EMAIL: {   // open email N, as the email panel does (tests)
            extern void read_email(int new_base, int num);
            printf("[host %u ms] email %d\n", now_ms(), s->a);
            read_email(0x989, s->a);   // EMAIL_BASE_ID (RES_email0, email.c)
            break;
        }
        case RES:
            report_res();
            break;
        case SFX: {   // the next effect each frame, until the last
            extern int play_digi_fx_master(int sfx_code, int num_loops, short id, unsigned short x, unsigned short y);
            int r = play_digi_fx_master(s->a, 1, 0, 0, 0);
            if (r < 0 || s->a == s->b) printf("[host %u ms] sfx %d: %d\n", now_ms(), s->a, r);
            if (s->a < s->b) { s->a++; next_step--; return; }   // again at the next poll
            break;
        }
        case QUIT:
            host_quit("quit");
            break;
#ifdef M20_SDL
        case WDOWN:
        case WUP:
            win_push_key(s->a, s->op == WDOWN);
            pushed = 1;
            break;
        case WMOVE:
            win_push_motion(s->a, s->b);
            pushed = 1;
            break;
        case WCLICK:
            win_push_button(s->a == 2 || s->a == -2 ? 1 : 0, s->a > 0);
            pushed = 1;
            break;
        case WQUIT:
            win_push_quit();
            pushed = 1;
            break;
#endif
        }
    }
}

// -------------------------------------------------------------------------
// The window's input (window.c's host_input), into the USB registers as a
// USB keyboard (port 0) and mouse (port 1) would report it. The port reads
// the registers once a game loop (usb_poll), so a key or button pressed and
// released between two reads would be lost: each poll takes the events in
// order up to the second change of a key or button (or a motion after a
// button change, so a click is where it was made), the rest at the next.
// -------------------------------------------------------------------------
enum { IN_KEY, IN_MOTION, IN_BUTTON, IN_SHOT, IN_QUIT, IN_RELEASE_ALL };
#define INQ 512
static struct inev { uint8_t type, a, b; int16_t x, y, dx, dy; } inq[INQ];
static int inq_n, want_quit, want_shot;

void host_input(int type, int a, int b, int x, int y, int dx, int dy) {
    if (type == IN_QUIT) { want_quit = 1; return; }
    if (type == IN_SHOT) { want_shot++; return; }
    if (inq_n == INQ) return;   // (the game has stopped reading)
    inq[inq_n++] = (struct inev){(uint8_t)type, (uint8_t)a, (uint8_t)b, (int16_t)x, (int16_t)y, (int16_t)dx, (int16_t)dy};
}

// The picture's coordinates (1280 x 720) to the game's screen and back:
// its columns between the video window's edges, its rows on all 720 lines
// (as screenshot() takes them).
static void pic_to_game(int px, int py, int *gx, int *gy) {
    uint32_t hw = VIDEO_HWIN, l = hw & 0xFFFFu, r = hw >> 16;
    int w = gScreenWide, h = gScreenHigh;
    if (r <= l || r > 1280u) l = 0, r = 1280u;
    int x = px < (int)l ? 0 : (int)(((uint32_t)px - l) * (uint32_t)w / (r - l));
    int y = py < 0 ? 0 : (int)((uint32_t)py * (uint32_t)h / 720u);
    *gx = x > w - 1 ? w - 1 : x;
    *gy = y > h - 1 ? h - 1 : y;
}

static void game_to_pic(int gx, int gy, int *px, int *py) {
    uint32_t hw = VIDEO_HWIN, l = hw & 0xFFFFu, r = hw >> 16;
    int w = gScreenWide > 0 ? gScreenWide : 1, h = gScreenHigh > 0 ? gScreenHigh : 1;
    if (r <= l || r > 1280u) l = 0, r = 1280u;
    *px = (int)l + (int)(((uint32_t)gx * 2u + 1u) * (r - l) / (2u * (uint32_t)w));
    *py = (int)(((uint32_t)gy * 2u + 1u) * 720u / (2u * (uint32_t)h));
}

// The game's mouselook on: the motion as deltas (the port's relative mode
// alone isn't it: the port turns it on again at the first motion after
// mouselook goes off, as sdl_events.c did)
static int game_relative(void) {
    extern int mlook_enabled;   // mouselook.c
    return mlook_enabled && SDL_GetRelativeMouseMode();
}

static void key_change(int hid, int down) {
    if (hid >= 0xE0 && hid <= 0xE7) {
        uint8_t bit = (uint8_t)(1u << (hid - 0xE0));
        held_mods = down ? held_mods | bit : held_mods & (uint8_t)~bit;
        return;
    }
    for (int i = 0; i < 4; i++)
        if (held[i] == hid) { if (!down) held[i] = 0; return; }
    if (down)   // (a fifth key isn't reported)
        for (int i = 0; i < 4; i++) if (!held[i]) { held[i] = (uint8_t)hid; return; }
}

static void apply_input(void) {
    uint8_t kchg[32] = {0}, bchg = 0;
    int keys = 0, abs_x = -1, abs_y = -1, dx = 0, dy = 0, i;
    int rel = game_relative();
    for (i = 0; i < inq_n; i++) {
        struct inev *e = &inq[i];
        if (e->type == IN_KEY) {
            if (kchg[e->a >> 3] & 1u << (e->a & 7u)) break;
            kchg[e->a >> 3] |= (uint8_t)(1u << (e->a & 7u));
            key_change(e->a, e->b);
            keys = 1;
        } else if (e->type == IN_BUTTON) {
            if (bchg & 1u << e->a) break;
            bchg |= (uint8_t)(1u << e->a);
            if (!rel) abs_x = e->x, abs_y = e->y;
            if (e->b) USB(1, 0x0C) |= 1u << e->a;
            else USB(1, 0x0C) &= ~(1u << e->a);
        } else if (e->type == IN_MOTION) {
            if (bchg) break;
            if (rel) dx += e->dx, dy += e->dy;
            else abs_x = e->x, abs_y = e->y;
        } else if (e->type == IN_RELEASE_ALL) {   // (the window lost the focus)
            if (keys || bchg) break;
            memset(held, 0, sizeof(held));
            held_mods = 0;
            keys = 1;
            USB(1, 0x0C) &= ~7u;
        }
    }
    memmove(inq, inq + i, sizeof(inq[0]) * (size_t)(inq_n - i));
    inq_n -= i;
    if (abs_x >= 0 || abs_y >= 0) {   // the pointer to where the host's is
        int gx, gy;
        pic_to_game(abs_x, abs_y, &gx, &gy);
        dx += gx - MouseX;
        dy += gy - MouseY;
    }
    if (dx || dy) {
        uint32_t pos = USB(1, 0x14);
        uint16_t x = (uint16_t)(pos + (uint32_t)dx), y = (uint16_t)((pos >> 16) + (uint32_t)dy);
        USB(1, 0x14) = (uint32_t)y << 16 | x;
    }
    if (keys) report_keys();
}

#ifdef M20_SDL
static void window_events(int poll) {
    static int rel;
    int r = game_relative();
    if (r != rel) {   // the game's mouselook on or off
        rel = r;
        int px, py;
        game_to_pic(MouseX, MouseY, &px, &py);
        win_set_relative(rel, px, py);
        printf("[host %u ms] mouselook %s\n", now_ms(), rel ? "on: the mouse's motion as deltas" : "off: the pointer where the host's is");
    }
    if (poll || pushed) win_poll();
    pushed = 0;
    apply_input();
    while (want_shot) {
        static int n;
        char name[32];
        snprintf(name, sizeof(name), "f12-%03d", n++);
        screenshot(name);
        want_shot--;
    }
    if (want_quit) host_quit("window closed");
}
#endif

// -------------------------------------------------------------------------
// The hardware's events, from the game's loop
// -------------------------------------------------------------------------
void __wrap_pump_events(void) {
    static uint32_t vb_last, au_last_us, mem_last;
#ifdef M20_SDL
    if (window && !frame_ms) {   // the window in real time: at most 60 frames a second (the rest would only heat the PC)
        extern uint32_t m9b_frames;
        static uint32_t frames_seen;
        static uint64_t frame_ns;
        if (m9b_frames != frames_seen) {
            frames_seen = m9b_frames;
            uint64_t t = now_ns(), next = frame_ns + 16666667u;
            if (t < next) {
                struct timespec ts = {0, (long)(next - t)};
                nanosleep(&ts, NULL);
                t = next;
            }
            frame_ns = t;
        }
    }
#endif
    if (frame_ms) {   // virtual time: a frame drawn since the last call takes at least frame_ms
        extern uint32_t m9b_frames;
        static uint32_t frames_seen;
        static uint64_t frame_start;
        if (m9b_frames != frames_seen) {
            frames_seen = m9b_frames;
            uint64_t next = frame_start + (uint64_t)frame_ms * CLOCK_PER_MS;
            if (vclock < next) vclock = next;
            frame_start = vclock;
        }
    }
    uint32_t t = now_ms();
    int vb = 0;
    if (t - vb_last >= 17u) {
        vb_last = t;
        vblank_flip();
        vb = 1;
#ifdef M20_SDL
        if (window) {   // the frame shown, as the HDMI output would show it
            static uint32_t *pic;
            if (!pic) pic = __real_malloc(4u * 1280u * 720u);
            picture(pic);
            win_present(pic);
        }
#endif
    }
    uint32_t us = (uint32_t)host_us();
    static int half;
    while (us - au_last_us >= 5333u) {   // 256 frames at 48 kHz
        au_last_us += 5333u;
        if (!AUDIO_CTRL) continue;
        sound_irq(half ? 4u : 2u);
        const uint32_t *buf = (const uint32_t *)(uintptr_t)(AUDIO_PTR(half) + PSRAM_BASE);
        if (wav) { fwrite(buf, 4, 256, wav); wav_bytes += 1024u; }
#ifdef M20_SDL
        if (window) win_audio(buf, 1024);
#endif
        half ^= 1;
    }
    static uint32_t files_last;
    if (t - files_last >= 200u) {
        files_last = t;
        int n = open_files();
        if (n > files_peak) files_peak = n;
    }
    if (t - mem_last >= 10000u) {
        mem_last = t;
        report_memory();
    }
    run_script();
#ifdef M20_SDL
    if (window) window_events(vb);
#endif
    (void)vb;
    __real_pump_events();
}

// The data directory: DIR itself if it holds res/, else its SSHOCK/ if that does
static char *find_data(const char *dir) {
    char b[1024];
    if (find_ci(dir, "res", b, sizeof(b))) return realpath(dir, NULL);
    if (find_ci(dir, "SSHOCK/res", b, sizeof(b)) && find_ci(dir, "SSHOCK", b, sizeof(b))) return realpath(b, NULL);
    return NULL;
}

extern int __real_main(int argc, char **argv);
int __wrap_main(int argc, char **argv) {
    window = getenv("M20_WINDOW") && atoi(getenv("M20_WINDOW"));
    // With the window the SCRIPT is optional: of two arguments, the second
    // is the SCRIPT if it's a file (OUTDIR SCRIPT), else DATADIR OUTDIR.
    struct stat st;
    int have_script = !window || argc == 4 || (argc == 3 && stat(argv[2], &st) == 0 && S_ISREG(st.st_mode));
    int nfixed = have_script ? 2 : 1;   // OUTDIR [SCRIPT]
    if (argc - 1 != nfixed && argc - 1 != nfixed + 1) {
        fprintf(stderr, "usage: %s [DATADIR] OUTDIR SCRIPT  (DATADIR: the card's root or its SSHOCK/; default $SSHOCK_DATA)\n"
                        "       M20_WINDOW=1 %s [DATADIR] OUTDIR [SCRIPT]  (a window)\n",
                argv[0], argv[0]);
        return 1;
    }
#ifndef M20_SDL
    if (window) { fprintf(stderr, "host: built without SDL2: no window (make host with SDL2's i386 development files)\n"); return 1; }
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((uintptr_t)arena < SDRAM_BASE || (uintptr_t)_end > SDRAM_END) {
        fprintf(stderr, "host: the image and heap (0x%x..%p) don't fit the SDRAM window 0x800000..0xFFFFFF\n",
                SDRAM_BASE, (void *)_end);
        return 1;
    }
    const char *data_arg = argc - 1 > nfixed ? argv[1] : getenv("SSHOCK_DATA");
    const char *out_arg = argv[argc - nfixed], *script = have_script ? argv[argc - 1] : NULL;
    if (!data_arg) { fprintf(stderr, "host: no DATADIR and no $SSHOCK_DATA\n"); return 1; }
    data_dir = find_data(data_arg);
    if (!data_dir) { fprintf(stderr, "host: %s holds neither res/ nor SSHOCK/res/\n", data_arg); return 1; }
    const char *cap = getenv("M9B_HEAP_KB");
    if (cap && (size_t)atoi(cap) * 1024u < heap_cap) heap_cap = (size_t)atoi(cap) * 1024u;
    if (getenv("SSHOCK_SHOT_FULL")) shot_full = atoi(getenv("SSHOCK_SHOT_FULL"));
    if (window) frame_ms = 0;   // the window: real time (unless SSHOCK_FRAME_MS)
    if (getenv("SSHOCK_FRAME_MS")) frame_ms = (uint32_t)atoi(getenv("SSHOCK_FRAME_MS"));
    mkdir(out_arg, 0777);
    out_dir = realpath(out_arg, NULL);
    if (!out_dir) { perror(out_arg); return 1; }
    char sb[1200];
    const char *save = getenv("SSHOCK_SAVE");
    if (!save) { snprintf(sb, sizeof(sb), "%s/SSHOCK", out_dir); save = sb; }
    mkdir(save, 0777);
    save_dir = realpath(save, NULL);
    if (!save_dir) { perror(save); return 1; }
    printf("[host] image 0x800000..%p, heap %p..%p (%u KB, cap %zu KB), free to 0x1000000: %u KB\n", (void *)arena,
           (void *)arena, (void *)(arena + M20_HEAP), M20_HEAP >> 10, heap_cap >> 10,
           (unsigned)((SDRAM_END - (uintptr_t)_end) >> 10));
    printf("[host] data %s, saves %s, output %s; time %s", data_dir, save_dir, out_dir, frame_ms ? "virtual" : "real\n");
    if (frame_ms) printf(", %u ms a frame at least\n", frame_ms);
    if (script) load_script(script);
    char path[1200];
    snprintf(path, sizeof(path), "%s/audio.wav", out_dir);
    wav = __real_fopen(path, "wb");
    if (wav) {
        static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                        16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0,
                                        4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        fwrite(hdr, 1, 44, wav);
    }
    if (chdir(save_dir)) { perror(save_dir); return 1; }
#ifdef M20_ENGINE
    span_emu_init((void *)(uintptr_t)SDRAM_BASE);
#endif
    atexit(report);
#ifdef M20_SDL
    if (window) {
        const char *a = getenv("SSHOCK_AUDIO");
        int fd0 = open_files();
        if (win_open("System Shock (m20 host)", !a || atoi(a))) return 1;
        fd_window = open_files() - fd0;
        printf("[host] window 1280 x 720 (F12: a screenshot in %s)\n", out_dir);
    }
#endif
    USB(0, 0x00) = 1u;   // a keyboard on port 0
    USB(1, 0x00) = 2u;   // a mouse on port 1
    return __real_main(1, argv);
}

// The board's UART screenshots (../../core/runtime/shot.c): none here (the
// script's "shot" saves the picture)
void m20_screenshot(uint32_t w, uint32_t h, const uint16_t *pal565, const uint8_t *pixels, uint32_t stride) {
    (void)w; (void)h; (void)pal565; (void)pixels; (void)stride;
}
void m20_screenshot16(uint32_t w, uint32_t h, const uint16_t *pixels, uint32_t stride) {
    (void)w; (void)h; (void)pixels; (void)stride;
}
