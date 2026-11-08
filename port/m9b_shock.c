// System Shock on the m20 SoC: start-up, video, time and files, in place of
// MacSrc/Shock.c and ShockBitmap.c (SDL).
//
// Video: the game draws into drawSurface (8 bpp, gScreenAddress; its 3D
// view goes through offscreenDrawSurface first). SDLDraw() copies the
// frame into one of three display buffers, cleans it from the data cache
// and has the vertical-blank interrupt show it from the next frame, with
// the palette loaded at the same time. The screen fills the 1280 x 720
// picture: its rows are spread over the 720 lines (320 x 200: 3 or 4 each,
// as DOS's 320 x 200 filled the screen), and its columns are doubled by the
// line table's 2x lines, and once more in the copy when the screen is 320
// wide (an 8 bpp line fetches up to 768 bytes).
//
// Palette: the game's 8-bit entries through Shock.c's gamma table (the
// gamma setting), as RGB565.
//
// Files: everything is under SSHOCK/ on the SD card (see m9b_posix.c).
//
// m9b_screenshot() (0x1C on the UART, see m9b_events.c) sends the screen
// for ../../core/tools/m9bctl.py shot (../../core/runtime/shot.c).
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "InitMac.h"
#include "Modding.h"
#include "OpenGL.h"
#include "Prefs.h"
#include "Shock.h"
#include "ShockBitmap.h"

#include "amaploop.h"
#include "gr2ss.h"
#include "hkeyfunc.h"
#include "mainloop.h"
#include "setup.h"
#include "shockolate_version.h"
#include "status.h"
#include "version.h"

#include "m9b_hw.h"
#include "m9b_map.h"

#define VRAM          ((volatile uint32_t*)M9B_VRAM)
#define VRAM_PALETTE  (VRAM + 512)   // 256 x RGB565, two per word
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t*)M9B_CTRL)
#define VIDEO_HWIN    (*(volatile uint32_t*)(M9B_CTRL + 0x18u))
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_STATUS   (*(volatile uint32_t*)(M9B_UART + 0x4u))
#define LINE_BLANK    (0u << 22)
#define LINE_8BPP     (2u << 22)
#define LINE_16BPP    (1u << 22)
#define LINE_2X       (1u << 28)

//--------------------
//  Shock.c's globals
//--------------------
bool gPlayingGame;
grs_screen *cit_screen;
SDL_Window *window;
SDL_Renderer *renderer;
SDL_AudioDeviceID device;
int num_args;
char **arg_values;
bool UseCutscenePalette = FALSE;   // see cutsloop.c
bool MouseCaptured = FALSE;
SDL_Surface *drawSurface;
SDL_Surface *offscreenDrawSurface;

extern grs_screen *svga_screen;
extern frc *svga_render_context;
extern void init_all(void);
extern errtype load_da_palette(void);
extern void CreateDefaultKeybindsFile(void);
extern void LoadHotkeyKeybinds(void);
extern void LoadMoveKeybinds(void);
extern int mlook_enabled;

// -------------------------------------------------------------------------
// Time
// -------------------------------------------------------------------------
Uint32 SDL_GetTicks(void) {
    static uint32_t last, ms, frac;   // CLOCK_CNT (54 MHz) wraps every 79 s
    uint32_t m = irq_off();           // (also called from the audio interrupt)
    uint32_t now = CLOCK_CNT;
    frac += now - last;
    last = now;
    ms += frac / CLOCK_PER_MS;
    frac %= CLOCK_PER_MS;
    uint32_t r = ms;
    irq_restore(m);
    return r;
}

extern void pump_events(void);

void SDL_Delay(Uint32 ms) {
    uint32_t t0 = SDL_GetTicks();
    do pump_events();   // keeps the music stream fed
    while (SDL_GetTicks() - t0 < ms);
}

// -------------------------------------------------------------------------
// Files (the paths: m9b_posix.c)
// -------------------------------------------------------------------------
char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org; (void)app;
    return strdup("");
}

char *SDL_GetBasePath(void) { return strdup(""); }

// -------------------------------------------------------------------------
// Video
// -------------------------------------------------------------------------
static int32_t scr_w, scr_h;
#define NBUF 3
static uint8_t *disp[NBUF];
static volatile int shown = -1, pending = -1;
static uint32_t pal_words[128];      // the palette in the hardware's format
static volatile uint32_t pal_dirty;  // load it at the next vertical blank
static volatile uint32_t vblanks;
#define LINES 720
static uint16_t row_of[LINES];       // the screen row each of the 720 lines shows
static uint32_t disp_w;              // a display buffer's row: scr_w, or 2 scr_w when doubled

static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

#ifdef M20_ENGINE
// The span engine's frames (port/m20_engine.c): a display buffer holds the
// rows the 3D view covers, RGB565, the engine's polygons with the screen's
// other pixels written over them (compose()); the line table shows the
// other rows straight from the game's screen (drawSurface, 8 bpp: the
// interface, which changes little; whole RGB565 frames don't fit in the
// heap). A frame without the engine's polygons is copied and shown 8 bpp
// throughout as before, unless the screen still has holes: then it's
// composed over the last engine frame ("world"). Only undoubled screens
// (640 wide) use the engine.
#include "m20_engine.h"
#define MAX_VIEW_ROWS 264
static uint8_t disp_fmt[NBUF];       // 1: the view's rows, RGB565
static uint32_t n_engine, n_over, n_plain;
static uint32_t m20_clean_cycles;   // compose's cleaning of the interface rows
static uint32_t m20_scan_cycles, m20_scan_written;   // its view scan, and the pixels it wrote   // frames: the engine's, composed over the last one, 8 bpp
static uint32_t view_y0, view_h;     // those rows
static uint32_t view_x, view_w;      // the view's columns in them (the rest: the interface)
static uint32_t side_sum[NBUF][MAX_VIEW_ROWS];   // each row's interface columns as a buffer has them (a checksum)
static uint16_t pal565[256];
static uint32_t pal888[256], pal_gen;
static int world = -1;               // the display buffer with the last engine frame
static int has_holes;
static int eng_b = -1;               // the display buffer the engine draws into

static uint32_t line_16bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_16BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

static void cache_flush(const void *p, uint32_t len) {
    for (uintptr_t a = (uintptr_t)p & ~63u; a < (uintptr_t)p + len; a += 64u)
        asm volatile("cbo.flush (%0)" :: "r"(a) : "memory");
}
#endif

// From the trap handler (../../core/runtime/trap.c) on each vertical blank.
void vblank_flip(void) {
    vblanks++;
    if (pal_dirty) {
        for (uint32_t i = 0; i < 128u; i++) VRAM_PALETTE[i] = pal_words[i];
        pal_dirty = 0;
    }
    int p = pending;
    if (p < 0) return;
    const uint8_t *buf = disp[p];
#ifdef M20_ENGINE
    if (disp_fmt[p]) {
        const uint8_t *screen = drawSurface->pixels;
        for (uint32_t y = 0; y < LINES; y++) {
            uint32_t r = row_of[y];
            VRAM_TABLE[y] = (r - view_y0 < view_h ? line_16bpp((uintptr_t)&buf[(r - view_y0) * disp_w * 2u])
                                                  : line_8bpp((uintptr_t)&screen[r * disp_w])) | LINE_2X;
        }
    } else
#endif
    for (uint32_t y = 0; y < LINES; y++)
        VRAM_TABLE[y] = line_8bpp((uintptr_t)&buf[row_of[y] * disp_w]) | LINE_2X;
    shown = p;
    pending = -1;
}

static SDL_Surface *new_surface(int w, int h) {
    SDL_Surface *s = malloc(sizeof(SDL_Surface));
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->pitch = w;
    s->pixels = aligned_alloc(64, ((size_t)w * h + 63u) & ~63u);
    if (!s->pixels) { free(s); return NULL; }
    memset(s->pixels, 0, (size_t)w * h);
    return s;
}

static void free_surface(SDL_Surface *s) {
    if (!s) return;
    free(s->pixels);
    free(s);
}

// The display buffers and the line table's rows for a w x h screen.
static void set_display(int w, int h) {
    if (w == scr_w && h == scr_h) return;
    uint32_t m = irq_off();
    pending = shown = -1;
    for (uint32_t y = 0; y < LINES; y++) VRAM_TABLE[y] = LINE_BLANK;
    irq_restore(m);
    if (w > 640 || h > (int)LINES || (w & 3)) { ERROR("Can't show a %dx%d screen", w, h); return; }
    uint32_t dw = w <= 320 ? 2u * (uint32_t)w : (uint32_t)w;
#ifdef M20_ENGINE
    size_t bytes = (size_t)dw * h;
    if (dw == (uint32_t)w && bytes < (size_t)MAX_VIEW_ROWS * 2u * w) bytes = (size_t)MAX_VIEW_ROWS * 2u * w;   // (the view's rows RGB565)
    world = -1;
    has_holes = 0;
#else
    size_t bytes = (size_t)dw * h;
#endif
    for (int i = 0; i < NBUF; i++) {
        free(disp[i]);
        disp[i] = aligned_alloc(256, (bytes + 255u) & ~255u);
        if (!disp[i]) { ERROR("No memory for a %dx%d screen", w, h); return; }
        memset(disp[i], 0, bytes);
#ifdef M20_ENGINE
        cache_flush(disp[i], (uint32_t)bytes);
        disp_fmt[i] = 0;
#else
        cache_clean(disp[i], (uint32_t)(dw * h));
#endif
    }
    scr_w = w;
    scr_h = h;
    disp_w = dw;
    for (uint32_t y = 0; y < LINES; y++) row_of[y] = (uint16_t)(y * (uint32_t)h / LINES);
    uint32_t cols = 2u * dw;   // centred in the 1280 columns
    VIDEO_HWIN = (640u + cols / 2u) << 16 | (640u - cols / 2u);
    VIDEO_CTRL = 1;
}

void SetupOffscreenBitmaps(int width, int height) {
    DEBUG("SetupOffscreenBitmaps %i %i", width, height);
    free_surface(drawSurface);
    free_surface(offscreenDrawSurface);
    drawSurface = new_surface(width, height);
    offscreenDrawSurface = new_surface(width, height);
    if (!drawSurface || !offscreenDrawSurface) {
        ERROR("No memory for the %dx%d draw surfaces", width, height);
        return;
    }
    gScreenRowbytes = drawSurface->w;
    gScreenAddress = drawSurface->pixels;
    grd_mode_cap.vbase = (uchar *)gScreenAddress;
    set_display(width, height);
}

void ChangeScreenSize(int width, int height) {
    if (gScreenWide == width && gScreenHigh == height) return;
    INFO("ChangeScreenSize %d x %d", width, height);
    SetupOffscreenBitmaps(width, height);
    gScreenWide = width;
    gScreenHigh = height;
}

void SDL_GetWindowSize(SDL_Window *w, int *width, int *height) {
    (void)w;
    *width = gScreenWide;
    *height = gScreenHigh;
}

uint32_t m9b_frames, m9b_copy_cycles;   // for the profile report

#ifdef M20_ENGINE
const uint32_t *m20_video_pal888(uint32_t *generation) {
    *generation = pal_gen;
    return pal888;
}

// A display buffer neither shown nor waiting to be (with two: the waiting
// one goes up at the next vertical blank)
static int free_buffer(void) {
    for (;;) {
        uint32_t m = irq_off();
        int b = 0;
        while (b < NBUF && (b == shown || b == pending)) b++;
        irq_restore(m);
        if (b < NBUF) return b;
    }
}

// The engine's target for this frame: a free display buffer (no line of
// which is in the data cache: see compose and SDLDraw), at the view's
// place on the screen (FrUtils.c: Fast_Slot_Copy), the view h rows high
uint16_t *m20_video_engine_target(uint32_t *stride, int w, int h) {
    extern uchar full_game_3d;
    if (disp_w != (uint32_t)scr_w || !disp[NBUF - 1] || full_game_3d || h > MAX_VIEW_ROWS) return NULL;
    int b = free_buffer();
    eng_b = b;
    int x = SCONV_X(28), y = SCONV_Y(24);
    if (y < 0 || y + h > scr_h) return NULL;
    if (x < 0 || x + w > scr_w) return NULL;
    if ((uint32_t)y != view_y0 || (uint32_t)h != view_h || (uint32_t)x != view_x || (uint32_t)w != view_w) {
        view_y0 = (uint32_t)y;   // (a new layout: the old frames can't be composed over)
        view_h = (uint32_t)h;
        view_x = (uint32_t)x;
        view_w = (uint32_t)w;
        world = -1;
        has_holes = 0;
        memset(side_sum, 0xFF, sizeof(side_sum));
    }
    *stride = (uint32_t)scr_w;
    return (uint16_t *)disp[b] + x;
}

// The frame in display buffer b: the view's rows as RGB565, the screen's
// pixels over the engine's (holes keep them); engine: this frame's are in
// b, else the last engine frame's are (in world). The other rows are shown
// from the screen: out of the data cache.
static void compose(int b, int engine) {
    const uint8_t *src = drawSurface->pixels;
    uint32_t w = (uint32_t)scr_w, holes = 0;
    uint8_t *dst = disp[b];
    uint32_t t0 = CLOCK_CNT;
    cache_clean(src, view_y0 * w);
    cache_clean(src + (view_y0 + view_h) * w, ((uint32_t)scr_h - view_y0 - view_h) * w);
    m20_clean_cycles += CLOCK_CNT - t0;
    uint16_t *v16 = (uint16_t *)dst;
    uint32_t n = view_h * w;
    int copied = !engine && b != world;
    if (copied) {
        memcpy(v16, disp[world], 2u * n);
        memcpy(side_sum[b], side_sum[world], sizeof(side_sum[b]));
    }
    uint32_t t1 = CLOCK_CNT, written = 0;
    for (uint32_t y = 0; y < view_h; y++) {
        const uint8_t *srow = src + (view_y0 + y) * w;
        uint16_t *drow = v16 + y * w;
        // the interface columns: only when they changed since this buffer had them
        uint32_t sum = 0;
        for (uint32_t x = 0; x < view_x; x += 4u) sum = sum * 31u + *(const uint32_t *)(srow + x);
        for (uint32_t x = view_x + view_w; x < w; x += 4u) sum = sum * 31u + *(const uint32_t *)(srow + x);
        if (sum != side_sum[b][y]) {
            side_sum[b][y] = sum;
            for (uint32_t x = 0; x < view_x; x++) drow[x] = pal565[srow[x]];
            for (uint32_t x = view_x + view_w; x < w; x++) drow[x] = pal565[srow[x]];
            cache_flush(drow, 2u * view_x);
            cache_flush(drow + view_x + view_w, 2u * (w - view_x - view_w));
            written += w - view_w;
        }
        // the view: four pixels at a time (all holes is the common case);
        // each line written leaves the data cache once done with
        const uint32_t *s4 = (const uint32_t *)(srow + view_x);
        uint16_t *dv = drow + view_x;
        uintptr_t line = 0;
        for (uint32_t i = 0; i < view_w / 4u; i++) {
            uint32_t v = s4[i];
            if (v == 0xFFFFFFFFu) { holes += 4; continue; }
            uint16_t *d = dv + 4u * i;
            uintptr_t l = (uintptr_t)d & ~(uintptr_t)63;
            if (l != line) {
                if (line) asm volatile("cbo.flush (%0)" :: "r"(line) : "memory");
                line = l;
            }
            for (uint32_t k = 0; k < 4u; k++, v >>= 8) {
                uint32_t c = v & 0xFFu;
                if (c == M20_HOLE) holes++;
                else { d[k] = pal565[c]; written++; }
            }
        }
        if (line) asm volatile("cbo.flush (%0)" :: "r"(line) : "memory");
    }
    m20_scan_cycles += CLOCK_CNT - t1;
    m20_scan_written += written;
    if (copied) cache_flush(v16, 2u * n);
    world = b;
    disp_fmt[b] = 1;
    has_holes = holes != 0;
}
#endif

void SDLDraw(void) {
    if (!drawSurface || !disp[NBUF - 1]) return;
#ifdef M20_ENGINE
    int b = m20_engine_frame ? eng_b : free_buffer();
#else
    int b = 0;
    while (b == shown || b == pending) b++;
#endif
    uint32_t c0 = CLOCK_CNT;
    uint32_t n = (uint32_t)(scr_w * scr_h);
#ifdef M20_ENGINE
    if (m20_engine_frame) {
        m20_engine_wait();
        m20_engine_frame = 0;
        compose(b, 1);
        n_engine++;
    } else if (has_holes && world >= 0 && disp_w == (uint32_t)scr_w) {
        compose(b, 0);
        n_over++;
    } else {
    disp_fmt[b] = 0;
    n_plain++;
#endif
    if (disp_w == (uint32_t)scr_w) {
        memcpy(disp[b], drawSurface->pixels, n);
    } else {   // each pixel twice: four in, two words out
        const uint32_t *src = (const uint32_t *)drawSurface->pixels;
        uint32_t *dst = (uint32_t *)disp[b];
        for (uint32_t i = 0; i < n / 4u; i++) {
            uint32_t v = src[i];
            uint32_t lo = (v & 0xFFu) | (v & 0xFF00u) << 8, hi = (v >> 16 & 0xFFu) | (v >> 8 & 0xFF0000u);
            dst[2u * i] = lo | lo << 8;
            dst[2u * i + 1u] = hi | hi << 8;
        }
    }
#ifdef M20_ENGINE
    cache_flush(disp[b], disp_w * (uint32_t)scr_h);
    }
#else
    cache_clean(disp[b], disp_w * (uint32_t)scr_h);
#endif
    m9b_copy_cycles += CLOCK_CNT - c0;
    uint32_t m = irq_off();
    pending = b;
    irq_restore(m);
    m9b_frames++;
#ifdef M9B_PROFILE
    extern void m9b_profile_frame(void);
    m9b_profile_frame();
#endif
}

void SetSDLPalette(int index, int count, uchar *pal) {
    // Shock.c's gamma table (factor 2.2: no OpenGL), for the current
    // setting only: powf is slow library code here (Shock.c made all 91).
    static int lut_gamma = -1;
    static uchar lut[256];
    int gam = gShockPrefs.doGamma;
    if (gam < 10) gam = 10;
    if (gam > 100) gam = 100;
    if (gam != lut_gamma) {
        float gamma = 1.0f - (float)gam / 100.0f;
        gamma = 1.0f - gamma * gamma;
        gamma = 1.0f / (gamma * 2.2f);
        for (int j = 0; j < 256; j++) lut[j] = (uchar)(powf((float)j / 255.0f, gamma) * 255.0f);
        lut_gamma = gam;
    }

    static uint16_t rgb565[256];
    for (int i = index; i < index + count && i < 256; i++) {
        uint32_t r = lut[*pal++], g = lut[*pal++], bl = lut[*pal++];
        rgb565[i] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3));
#ifdef M20_ENGINE
        pal888[i] = r << 16 | g << 8 | bl;
#endif
    }
    if (!UseCutscenePalette) rgb565[255] = 0;   // Shock.c: "Hack black!"
#ifdef M20_ENGINE
    if (!UseCutscenePalette) pal888[255] = 0;
    memcpy(pal565, rgb565, sizeof(pal565));
    pal_gen++;
#endif
    uint32_t words[128];
    for (int i = 0; i < 128; i++) words[i] = rgb565[2 * i] | (uint32_t)rgb565[2 * i + 1] << 16;
    uint32_t m = irq_off();
    memcpy(pal_words, words, sizeof(words));
    pal_dirty = 1;
    irq_restore(m);
}

void InitSDL(void) {
    gr_init();
    extern short svga_mode_data[];
    gr_set_mode(svga_mode_data[gShockPrefs.doVideoMode], TRUE);
    INFO("Setting up screen and render contexts");
    SetupOffscreenBitmaps(grd_cap->w, grd_cap->h);
    gScreenWide = grd_cap->w;
    gScreenHigh = grd_cap->h;
    svga_screen = cit_screen = gr_alloc_screen(grd_cap->w, grd_cap->h);
    gr_set_screen(svga_screen);
    gr_alloc_ipal();
    SDLDraw();
}

void CaptureMouse(bool capture) {
    MouseCaptured = capture && gShockPrefs.goCaptureMouse;
    if (!MouseCaptured && mlook_enabled && SDL_GetRelativeMouseMode() == SDL_TRUE) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_WarpMouseInWindow(window, gScreenWide / 2, gScreenHigh / 2);
    } else {
        SDL_SetRelativeMouseMode(MouseCaptured ? SDL_TRUE : SDL_FALSE);
    }
}

#ifdef M9B_PROFILE
// Every 5 s: frames per second, time in the sound interrupt, the frame copy
// and file reads, misaligned accesses emulated (../../core/runtime/trap.c), the
// heap's top; every 15 s the PC samples (../../core/tools/prof.py).
void m9b_profile_frame(void) {
    extern uint32_t sound_cycles, misaligned_traps, m9b_read_cycles, m9b_read_bytes;
    extern void prof_report(void);
    extern char __heap_start;
    static uint32_t frames0, t0, snd0, mis0, cp0, rd0, rb0, reports;
    uint32_t now = CLOCK_CNT, dt = now - t0;
    if (!t0) {
        t0 = now;
        frames0 = m9b_frames;
        return;
    }
    if (dt < 5u * CLOCK_HZ) return;
    uint32_t fr = m9b_frames - frames0;
#ifdef M9B_DIV_STATS
    extern uint32_t m9b_div_fast, m9b_div_lu, m9b_div_slow;
    printf("[div 32-bit %lu, 64-bit %lu, libgcc %lu] ", (unsigned long)m9b_div_fast, (unsigned long)m9b_div_lu,
           (unsigned long)m9b_div_slow);
    m9b_div_fast = m9b_div_lu = m9b_div_slow = 0;
#endif
    printf("%lu.%lu fps, sound %lu%%, frame copy %lu%%, reads %lu%% (%lu KB), %lu misaligned, heap top %lu KB\n",
           (unsigned long)(fr * (10ull * CLOCK_HZ) / dt / 10), (unsigned long)(fr * (10ull * CLOCK_HZ) / dt % 10),
           (unsigned long)((uint64_t)(sound_cycles - snd0) * 100 / dt),
           (unsigned long)((uint64_t)(m9b_copy_cycles - cp0) * 100 / dt),
           (unsigned long)((uint64_t)(m9b_read_cycles - rd0) * 100 / dt), (unsigned long)((m9b_read_bytes - rb0) / 1024),
           (unsigned long)(misaligned_traps - mis0), (unsigned long)(((char *)sbrk(0) - &__heap_start) / 1024));
#ifdef M20_ENGINE
    {   // the engine's counters (../../core/rtl/span_engine.v): pixels, texel misses, busy clocks
        volatile uint32_t *sp = (volatile uint32_t *)0x490000u;
        static uint32_t p0, mi0, b0;
        uint32_t pp = sp[21], mi = sp[22], bb = sp[23];
        printf("  frames: %u engine, %u over the last, %u 8 bpp; cleaning the interface %u%%, the view scan %u%% (%u pixels written)\n", (unsigned)n_engine,
               (unsigned)n_over, (unsigned)n_plain, (unsigned)((uint64_t)m20_clean_cycles * 100 / dt),
               (unsigned)((uint64_t)m20_scan_cycles * 100 / dt), (unsigned)m20_scan_written);
        m20_scan_cycles = m20_scan_written = 0;
        n_engine = n_over = n_plain = 0;
        m20_clean_cycles = 0;
        printf("  engine %s: %u polygons, %u spans, %u pixels, %u misses, busy %u%%, CPU waits %u%%\n",
               m20_engine_enable ? "on" : "off", m20_engine_faces, m20_engine_spans, (unsigned)(pp - p0),
               (unsigned)(mi - mi0), (unsigned)((uint64_t)(bb - b0) * 100 / dt),
               (unsigned)((uint64_t)m20_engine_wait_cycles * 100 / dt));
        p0 = pp; mi0 = mi; b0 = bb;
        extern unsigned int m20_tmap_cycles;
        printf("  engine polygons (setup and spans) %u%% of the time\n", (unsigned)((uint64_t)m20_tmap_cycles * 100 / dt));
        m20_tmap_cycles = 0;
        m20_engine_faces = m20_engine_spans = m20_engine_wait_cycles = 0;
    }
#endif
    t0 = now;
    frames0 = m9b_frames;
    snd0 = sound_cycles;
    mis0 = misaligned_traps;
    cp0 = m9b_copy_cycles;
    rd0 = m9b_read_cycles;
    rb0 = m9b_read_bytes;
    if (++reports % 3u == 0) prof_report();
}
#endif

// -------------------------------------------------------------------------
// Screenshots over the UART
// -------------------------------------------------------------------------

extern void m20_screenshot(uint32_t w, uint32_t h, const uint16_t *pal565,
                           const uint8_t *pixels, uint32_t stride);   // ../../core/runtime/shot.c

extern void m20_screenshot16(uint32_t w, uint32_t h, const uint16_t *pixels, uint32_t stride);

void m20_screenshot_screen(void) {
    if (drawSurface) m20_screenshot((uint32_t)scr_w, (uint32_t)scr_h, (const uint16_t *)pal_words, drawSurface->pixels, (uint32_t)scr_w);
}

void m9b_screenshot(void) {
#ifdef M20_ENGINE
    if (shown >= 0 && disp_fmt[shown]) {   // (the view's rows: the RGB565 part)
        const uint8_t *v = disp[shown];
        m20_screenshot16((uint32_t)scr_w, view_h, (const uint16_t *)v, (uint32_t)scr_w);
        cache_flush(v, 2u * view_h * (uint32_t)scr_w);
        return;
    }
#endif
    // (a doubled display buffer has every pixel twice: the draw surface then)
    const uint8_t *px = shown >= 0 && disp_w == (uint32_t)scr_w ? disp[shown]
                        : drawSurface ? drawSurface->pixels : NULL;
    if (px) m20_screenshot((uint32_t)scr_w, (uint32_t)scr_h, (const uint16_t *)pal_words, px, (uint32_t)scr_w);
}

// -------------------------------------------------------------------------
// main (Shock.c's)
// -------------------------------------------------------------------------
bool CheckArgument(char *arg) {
    if (arg == NULL) return false;
    for (int i = 1; i < num_args; i++)
        if (strcmp(arg_values[i], arg) == 0) return true;
    return false;
}

int main(int argc, char **argv) {
    static char *args[] = {"shock", NULL};
    (void)argc; (void)argv;
    num_args = 1;
    arg_values = args;

    hw_start(1u);   // the vertical-blank interrupt (page flips); the audio ones come with the sound

    log_set_quiet(0);
    log_set_level(LOG_INFO);
    INFO("%s on the m9b", SHOCKOLATE_VERSION);

    InitMac();
    SetDefaultPrefs();
    LoadPrefs();
    CreateDefaultKeybindsFile();
    LoadHotkeyKeybinds();
    LoadMoveKeybinds();

    bool show_splash = !CheckArgument("-nosplash");
    ProcessModArgs(num_args, arg_values);

    init_all();
    setup_init();
    // How far (in screen pixels) the pointer may move between the presses
    // of a double click, and between press and release of a click (the 3D
    // view's "look at"); the game's 5 was made for DOS, where a mouse moved
    // the pointer about one 320 x 200 pixel per 2 counts. Here a pixel is
    // one USB mouse count (m9b_events.c), so 5 pixels is about 0.1 mm of hand
    // movement and double clicks mostly failed.
    uiDoubleClickTolerance = 16;
    gPlayingGame = true;
    load_da_palette();
    gr_clear(0xFF);

    INFO("Showing splash screen");
    splash_draw(show_splash);

    _new_mode = _current_loop = SETUP_LOOP;
    loopmode_enter(SETUP_LOOP);

    INFO("Showing main menu, starting game loop");
    mainloop(num_args, arg_values);

    status_bio_end();
    stop_music();
    return 0;
}
