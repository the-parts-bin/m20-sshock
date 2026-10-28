// System Shock on the m9b SoC: start-up, video, time and files, in place of
// MacSrc/Shock.c and ShockBitmap.c (SDL).
//
// Video: the game draws into drawSurface (8 bpp, gScreenAddress; its 3D
// view goes through offscreenDrawSurface first). SDLDraw() copies the
// frame into one of three display buffers, cleans it from the data cache
// and has the vertical-blank interrupt show it from the next frame, with
// the palette loaded at the same time. 320-wide modes are shown with
// columns doubled; every mode's lines are stretched to all 480 (320 x 200:
// 2 or 3 each, as DOS's 320 x 200 filled a 4:3 screen). 640 x 480 is shown
// as it is.
//
// Palette: the game's 8-bit entries through Shock.c's gamma table (the
// gamma setting), as RGB565.
//
// Files: everything is under SSHOCK/ on the SD card (see m9b_posix.c).
//
// m9b_screenshot() (0x1C on the UART, see m9b_events.c) dumps the screen:
// "\nM9BSHOT\n", the width and height (16-bit), 256 RGB565 palette entries
// (little-endian), then the pixels of the buffer on screen
// (../../core/tools/m9bctl.py shot).
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
    static uint32_t last, ms, frac;   // CLOCK_CNT (36 MHz) wraps every 119 s
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
static uint16_t row_of[480];         // the screen row each of the 480 lines shows

static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

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
    uint32_t x2 = scr_w <= 360 ? LINE_2X : 0u;
    for (uint32_t y = 0; y < 480u; y++)
        VRAM_TABLE[y] = line_8bpp((uintptr_t)&buf[row_of[y] * scr_w]) | x2;
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
    for (uint32_t y = 0; y < 480u; y++) VRAM_TABLE[y] = LINE_BLANK;
    irq_restore(m);
    for (int i = 0; i < NBUF; i++) {
        free(disp[i]);
        disp[i] = aligned_alloc(256, ((size_t)w * h + 255u) & ~255u);
        if (!disp[i]) { ERROR("No memory for a %dx%d screen", w, h); return; }
        memset(disp[i], 0, (size_t)w * h);
        cache_clean(disp[i], (uint32_t)(w * h));
    }
    scr_w = w;
    scr_h = h;
    for (uint32_t y = 0; y < 480u; y++) row_of[y] = (uint16_t)(y * (uint32_t)h / 480u);
    uint32_t cols = w <= 360 ? 2u * (uint32_t)w : (uint32_t)w;
    if (cols > 720u) cols = 720u;
    uint32_t left = (720u - cols) / 2u;
    VIDEO_HWIN = (left + cols) << 16 | left;
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

void SDLDraw(void) {
    if (!drawSurface || !disp[NBUF - 1]) return;
    int b = 0;
    while (b == shown || b == pending) b++;
    uint32_t c0 = CLOCK_CNT;
    uint32_t n = (uint32_t)(scr_w * scr_h);
    memcpy(disp[b], drawSurface->pixels, n);
    cache_clean(disp[b], n);
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
    }
    if (!UseCutscenePalette) rgb565[255] = 0;   // Shock.c: "Hack black!"
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
    if (dt < 5u * 36000000u) return;
    uint32_t fr = m9b_frames - frames0;
#ifdef M9B_DIV_STATS
    extern uint32_t m9b_div_fast, m9b_div_lu, m9b_div_slow;
    printf("[div 32-bit %lu, 64-bit %lu, libgcc %lu] ", (unsigned long)m9b_div_fast, (unsigned long)m9b_div_lu,
           (unsigned long)m9b_div_slow);
    m9b_div_fast = m9b_div_lu = m9b_div_slow = 0;
#endif
    printf("%lu.%lu fps, sound %lu%%, frame copy %lu%%, reads %lu%% (%lu KB), %lu misaligned, heap top %lu KB\n",
           (unsigned long)(fr * 360000000ull / dt / 10), (unsigned long)(fr * 360000000ull / dt % 10),
           (unsigned long)((uint64_t)(sound_cycles - snd0) * 100 / dt),
           (unsigned long)((uint64_t)(m9b_copy_cycles - cp0) * 100 / dt),
           (unsigned long)((uint64_t)(m9b_read_cycles - rd0) * 100 / dt), (unsigned long)((m9b_read_bytes - rb0) / 1024),
           (unsigned long)(misaligned_traps - mis0), (unsigned long)(((char *)sbrk(0) - &__heap_start) / 1024));
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
static void uart_raw(uint8_t c) {
    while (UART_STATUS & 1u) {
    }
    UART_DATA = c;
}

void m9b_screenshot(void) {
    static const char tag[] = "\nM9BSHOT\n";
    for (const char *s = tag; *s; s++) uart_raw((uint8_t)*s);
    uart_raw((uint8_t)scr_w); uart_raw((uint8_t)(scr_w >> 8));
    uart_raw((uint8_t)scr_h); uart_raw((uint8_t)(scr_h >> 8));
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t w = pal_words[i];
        uart_raw((uint8_t)w); uart_raw((uint8_t)(w >> 8));
        uart_raw((uint8_t)(w >> 16)); uart_raw((uint8_t)(w >> 24));
    }
    const uint8_t *px = shown >= 0 ? disp[shown] : drawSurface ? drawSurface->pixels : NULL;
    for (int32_t i = 0; px && i < scr_w * scr_h; i++) uart_raw(px[i]);
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
