// A host (x86, 32-bit Linux) test harness for the port: runs the same game
// and port code (port/*.c but m9b_posix.c) without the board.
//
// The SoC's I/O window (0x400000..0x7FFFFF) is mapped as plain memory
// at its own address, so the port's register accesses land in it: a thread
// keeps CLOCK_CNT counting at 36 MHz, and the wrapped pump_events() (the
// game calls it every loop) plays the hardware's other part:
//  - every 16.7 ms a vertical blank (vblank_flip(), as the interrupt would),
//  - every 5.33 ms an audio half (sound_irq()), appended to OUT/audio.wav
//    (48 kHz 16-bit stereo, what the HDMI audio would get),
//  - the script's input, through the USB registers (keyboard on port 0,
//    mouse on port 1, as the hardware reports them),
//  - screenshots of the screen (drawSurface with the palette the vertical
//    blank loaded) to OUT/NAME.ppm.
// The game's allocations (malloc and friends, wrapped) are limited to what
// the board's heap holds (HEAP_KB, about what the memory map in ../../Makefile
// leaves after .data, .bss and the stack; M9B_HEAP_KB in the environment
// changes it), and the most in use is reported. New memory is filled
// with 0xA5 (the board's heap holds whatever was in PSRAM before).
//
// usage: sshock-host DATADIR OUTDIR SCRIPT
//   DATADIR holds the card's SSHOCK/ contents (res/data/..., res/sound/...)
//   with lower-case names (Linux is case-sensitive, FatFs isn't).
//   SCRIPT lines: "MS key HID" (press and release, 100 ms), "MS down HID",
//   "MS up HID", "MS move DX DY", "MS click [left|right]", "MS shot NAME",
//   "MS email N" (opens email N), "MS quit"; MS is the time since start in milliseconds, HID a USB HID
//   usage (hex with 0x, e.g. 0x28 Enter, 0x29 Esc, 0x52 Up). '#' comments.
#define _GNU_SOURCE
#include <dirent.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "SDL.h"
#include "m9b_map.h"

#define IO_BASE   M9B_IO_BASE
#define IO_SIZE   0x00400000u
#define REG(a)    (*(volatile uint32_t*)(uintptr_t)(a))
#define CLOCK_CNT REG((M9B_TIMING + 0xCu))
#define VRAM_PALETTE ((volatile uint32_t*)(uintptr_t)(M9B_VRAM + 0x800u))
#define AUDIO_CTRL REG((M9B_CTRL + 0xCu))
#define AUDIO_PTR(h) REG((M9B_CTRL + 0x10u) + 4u * (h))
#define USB(p, o) REG(M9B_USB + 0x20u * (p) + (o))
#define PSRAM_BASE M9B_PSRAM_BASE

extern void __real_pump_events(void);
extern void vblank_flip(void);
extern void sound_irq(uint32_t pending);
extern SDL_Surface *drawSurface;

#define HEAP_KB 3700u

static uint64_t t_start;
static const char *out_dir;
static FILE *wav;
static uint32_t wav_bytes;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint32_t now_ms(void) { return (uint32_t)((now_ns() - t_start) / 1000000u); }

static void *clock_thread(void *arg) {
    (void)arg;
    for (;;) {
        CLOCK_CNT = (uint32_t)((now_ns() - t_start) * 36u / 1000u);
        usleep(50);
    }
    return NULL;
}

__attribute__((constructor)) static void map_io(void) {
    void *p = mmap((void *)(uintptr_t)IO_BASE, IO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)IO_BASE) {
        perror("mapping the I/O window");
        exit(1);
    }
    t_start = now_ns();
    pthread_t t;
    pthread_create(&t, NULL, clock_thread, NULL);
}

// -------------------------------------------------------------------------
// Script
// -------------------------------------------------------------------------
enum { KEY, DOWN, UP, MOVE, CLICK, SHOT, EMAIL, QUIT };
static struct step { uint32_t ms; int op, a, b; char name[64]; } *steps;
static int nsteps, next_step;

static void load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char op[16] = "", a[64] = "", b[64] = "";
        unsigned ms;
        if (sscanf(line, "%u %15s %63s %63s", &ms, op, a, b) < 2) continue;
        steps = realloc(steps, sizeof(*steps) * (nsteps + 2));
        struct step *s = &steps[nsteps++];
        memset(s, 0, sizeof(*s));
        s->ms = ms;
        if (!strcmp(op, "key")) { s->op = KEY; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "down")) { s->op = DOWN; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "up")) { s->op = UP; s->a = (int)strtol(a, NULL, 0); }
        else if (!strcmp(op, "move")) { s->op = MOVE; s->a = atoi(a); s->b = atoi(b); }
        else if (!strcmp(op, "click")) { s->op = CLICK; s->a = !strcmp(a, "right") ? 2 : 1; }
        else if (!strcmp(op, "shot")) { s->op = SHOT; snprintf(s->name, sizeof(s->name), "%s", a); }
        else if (!strcmp(op, "email")) { s->op = EMAIL; s->a = atoi(a); }
        else if (!strcmp(op, "quit")) s->op = QUIT;
        else { fprintf(stderr, "script: unknown '%s'\n", op); exit(1); }
        if (s->op == KEY || s->op == CLICK) {   // the release, 100 ms later
            struct step *r = &steps[nsteps++];
            *r = *s;
            r->ms = s->ms + 100u;
            r->op = s->op == KEY ? UP : CLICK;
            r->a = s->op == KEY ? s->a : -s->a;
            s->op = s->op == KEY ? DOWN : CLICK;
        }
    }
    fclose(f);
    for (int i = 1; i < nsteps; i++)   // stable sort by time
        for (int j = i; j > 0 && steps[j - 1].ms > steps[j].ms; j--) {
            struct step t = steps[j]; steps[j] = steps[j - 1]; steps[j - 1] = t;
        }
}

// Keyboard: up to 4 keys held, as the USB registers report them.
static uint8_t held[4];

static void report_keys(void) {
    USB(0, 0x04) = (uint32_t)held[0] << 8 | (uint32_t)held[1] << 16 | (uint32_t)held[2] << 24;
    USB(0, 0x08) = held[3];
    USB(0, 0x00) = 1u | ((USB(0, 0x00) >> 8) + 1u & 0xFFu) << 8;   // keyboard, next report
}

static void screenshot(const char *name) {
    if (!drawSurface) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    int w = drawSurface->w, h = drawSurface->h;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const uint8_t *px = drawSurface->pixels;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = VRAM_PALETTE[px[i] / 2u] >> (16u * (px[i] & 1u)) & 0xFFFFu;
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)((c >> 5 & 63u) << 2), (uint8_t)((c & 31u) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("[host %u ms] shot %s\n", now_ms(), path);
}

// -------------------------------------------------------------------------
// The heap, limited to the board's
// -------------------------------------------------------------------------
extern void *__real_malloc(size_t n);
extern void *__real_calloc(size_t n, size_t m);
extern void *__real_realloc(void *p, size_t n);
extern void *__real_aligned_alloc(size_t a, size_t n);
extern void __real_free(void *p);
static size_t heap_used, heap_peak, heap_cap = HEAP_KB * 1024u;
static unsigned heap_refused;

static int heap_take(size_t n) {
    if (heap_used + n > heap_cap) {
        if (heap_refused++ < 20) printf("[host %u ms] malloc of %zu bytes refused (%zu KB in use)\n", now_ms(), n, heap_used / 1024);
        return 0;
    }
    heap_used += n;
    if (heap_used > heap_peak) heap_peak = heap_used;
    return 1;
}

static void *heap_got(void *p) {
    if (p) {
        heap_used += malloc_usable_size(p);
        if (heap_used > heap_peak) heap_peak = heap_used;
    }
    return p;
}

void *__wrap_malloc(size_t n) {
    if (!heap_take(n)) return NULL;
    heap_used -= n;
    void *p = heap_got(__real_malloc(n));
    if (p) memset(p, 0xA5, n);
    return p;
}

void *__wrap_calloc(size_t n, size_t m) {
    if (!heap_take(n * m)) return NULL;
    heap_used -= n * m;
    return heap_got(__real_calloc(n, m));
}

void *__wrap_aligned_alloc(size_t a, size_t n) {
    if (!heap_take(n)) return NULL;
    heap_used -= n;
    void *p = heap_got(__real_aligned_alloc(a, n));
    if (p) memset(p, 0xA5, n);
    return p;
}

void __wrap_free(void *p) {
    if (p) heap_used -= malloc_usable_size(p);
    __real_free(p);
}

void *__wrap_realloc(void *p, size_t n) {
    size_t old = p ? malloc_usable_size(p) : 0;
    if (n > old && !heap_take(n - old)) return NULL;
    if (n > old) heap_used -= n - old;
    heap_used -= old;
    void *q = __real_realloc(p, n);
    if (!q) { heap_used += old; return NULL; }
    if (n > old) memset((char *)q + old, 0xA5, n - old);
    return heap_got(q);
}

// Files open now (the board has 40: ../../core/runtime/syscalls.c's MAX_FILES).
static int open_files(void) {
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n - 4;   // stdin, stdout, stderr and the directory itself
}

static int files_peak;

static void report_memory(void) {
    printf("[host %u ms] heap in use %zu KB, most %zu KB, %u refused (cap %zu KB); %d files open, most %d\n", now_ms(),
           heap_used / 1024, heap_peak / 1024, heap_refused, heap_cap / 1024, open_files(), files_peak);
}

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
        case EMAIL: {   // open email N, as the email panel does (tests)
            extern void read_email(int new_base, int num);
            printf("[host %u ms] email %d\n", now_ms(), s->a);
            read_email(0x989, s->a);   // EMAIL_BASE_ID (RES_email0, email.c)
            break;
        }
        case QUIT:
            report_memory();
            if (wav) {   // the sizes in the header
                fseek(wav, 4, SEEK_SET);
                uint32_t riff = 36u + wav_bytes;
                fwrite(&riff, 4, 1, wav);
                fseek(wav, 40, SEEK_SET);
                fwrite(&wav_bytes, 4, 1, wav);
                fclose(wav);
            }
            printf("[host %u ms] quit\n", now_ms());
            exit(0);
        }
    }
}

// -------------------------------------------------------------------------
// The hardware's events, from the game's loop
// -------------------------------------------------------------------------
void __wrap_pump_events(void) {
    static uint32_t vb_last, au_last_us, mem_last;
    uint32_t t = now_ms();
    if (t - vb_last >= 17u) {
        vb_last = t;
        vblank_flip();
    }
    uint32_t us = (uint32_t)((now_ns() - t_start) / 1000u);
    static int half;
    while (us - au_last_us >= 5333u) {   // 256 frames at 48 kHz
        au_last_us += 5333u;
        if (!AUDIO_CTRL) continue;
        sound_irq(half ? 4u : 2u);
        const uint32_t *buf = (const uint32_t *)(uintptr_t)(AUDIO_PTR(half) + PSRAM_BASE);
        if (wav) { fwrite(buf, 4, 256, wav); wav_bytes += 1024u; }
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
    __real_pump_events();
}

extern int __real_main(int argc, char **argv);
int __wrap_main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s DATADIR OUTDIR SCRIPT\n", argv[0]);
        return 1;
    }
    const char *cap = getenv("M9B_HEAP_KB");
    if (cap) heap_cap = (size_t)atoi(cap) * 1024u;
    out_dir = realpath(argv[2], NULL);
    if (!out_dir) { perror(argv[2]); return 1; }
    load_script(argv[3]);
    char path[512];
    snprintf(path, sizeof(path), "%s/audio.wav", out_dir);
    wav = fopen(path, "wb");
    if (wav) {
        static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                        16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0,
                                        4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        fwrite(hdr, 1, 44, wav);
    }
    if (chdir(argv[1])) { perror(argv[1]); return 1; }
    USB(0, 0x00) = 1u;   // a keyboard on port 0
    USB(1, 0x00) = 2u;   // a mouse on port 1
    return __real_main(1, argv);
}

// The board's UART screenshots (../../core/runtime/shot.c): none here (the
// script's "shot" saves the screen)
void m20_screenshot(uint32_t w, uint32_t h, const uint16_t *pal565, const uint8_t *pixels, uint32_t stride) {
    (void)w; (void)h; (void)pal565; (void)pixels; (void)stride;
}
void m20_screenshot16(uint32_t w, uint32_t h, const uint16_t *pixels, uint32_t stride) {
    (void)w; (void)h; (void)pixels; (void)stride;
}
