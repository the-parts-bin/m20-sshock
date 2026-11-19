// The host harness's window (M20_WINDOW=1, built when SDL2 is found: the
// Makefile's HOST_SDL): shows the 1280 x 720 picture host.c builds from the
// line table and palette, as the board's HDMI output would, and turns SDL's
// keyboard and mouse events into host.c's input queue (host_input), which
// host.c feeds to the port through the USB registers. Optionally plays the
// audio halves live (SDL_QueueAudio).
//
// This file is built with SDL2's own headers, not the port's stand-in
// (port/SDL.h), and has no game includes. The port defines some SDL
// functions itself (SDL_GetTicks, SDL_Delay, SDL_SetRelativeMouseMode,
// SDL_ShowCursor, SDL_WarpMouseInWindow, SDL_PauseAudioDevice, ...), which
// the static link binds every call by that name to; the ones needed here
// are SDL2's, looked up with dlsym(RTLD_NEXT) (sdl_fn below). libSDL2 itself
// calls none of its public functions by name, so the port's don't reach it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#define PIC_W 1280
#define PIC_H 720

// host.c's side
enum { IN_KEY, IN_MOTION, IN_BUTTON, IN_SHOT, IN_QUIT, IN_RELEASE_ALL };
extern void host_input(int type, int a, int b, int x, int y, int dx, int dy);

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio;

static void *sdl_fn(const char *name) {   // SDL2's own function of a name the port also defines
    void *f = dlsym(RTLD_NEXT, name);
    if (!f) fprintf(stderr, "window: no %s in libSDL2 (%s)\n", name, dlerror());
    return f;
}

static int (*real_SetRelativeMouseMode)(SDL_bool);
static int (*real_ShowCursor)(int);
static void (*real_WarpMouseInWindow)(SDL_Window *, int, int);
static void (*real_PauseAudioDevice)(SDL_AudioDeviceID, int);

int win_open(const char *title, int with_audio) {
    real_SetRelativeMouseMode = (int (*)(SDL_bool))sdl_fn("SDL_SetRelativeMouseMode");
    real_ShowCursor = (int (*)(int))sdl_fn("SDL_ShowCursor");
    real_WarpMouseInWindow = (void (*)(SDL_Window *, int, int))sdl_fn("SDL_WarpMouseInWindow");
    real_PauseAudioDevice = (void (*)(SDL_AudioDeviceID, int))sdl_fn("SDL_PauseAudioDevice");
    if (!real_SetRelativeMouseMode || !real_ShowCursor || !real_WarpMouseInWindow || !real_PauseAudioDevice) return -1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "window: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, PIC_W, PIC_H, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "window: SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "window: SDL_CreateRenderer: %s\n", SDL_GetError()); return -1; }
    // The picture's 1280 x 720 coordinates whatever the window's size (SDL
    // scales the mouse events' positions to them too), letterboxed
    SDL_RenderSetLogicalSize(ren, PIC_W, PIC_H);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, PIC_W, PIC_H);
    if (!tex) { fprintf(stderr, "window: SDL_CreateTexture: %s\n", SDL_GetError()); return -1; }
    real_ShowCursor(SDL_DISABLE);   // the game draws its own pointer, where the host's is
    if (with_audio) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want = {0}, have;
            want.freq = 48000;
            want.format = AUDIO_S16SYS;
            want.channels = 2;
            want.samples = 1024;
            audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        }
        if (!audio) fprintf(stderr, "window: no sound (%s)\n", SDL_GetError());
        else real_PauseAudioDevice(audio, 0);
    }
    return 0;
}

void win_close(void) {
    if (audio) SDL_CloseAudioDevice(audio);
    audio = 0;
    if (win) SDL_Quit();
    win = NULL;
}

void win_present(const uint32_t *pic) {   // 1280 x 720, 0xRRGGBB
    SDL_UpdateTexture(tex, NULL, pic, PIC_W * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

// The audio halves (256 frames, 48 kHz 16-bit stereo) as they're mixed:
// queued, at most 200 ms ahead (the rest dropped: after a stall, or in
// virtual time, which runs faster than real time).
void win_audio(const void *buf, int bytes) {
    if (!audio) return;
    if (SDL_GetQueuedAudioSize(audio) > 48000u * 4u / 5u) return;
    SDL_QueueAudio(audio, buf, (Uint32)bytes);
}

// The game's mouselook (relative mode) on or off: SDL's relative mode
// (the pointer hidden and held in the window, motion as deltas) with it;
// back to absolute with the host pointer put where the game's is (x, y in
// picture coordinates).
void win_set_relative(int on, int x, int y) {
    real_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    if (!on) {
        // logical (picture) coordinates to the window's
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        float s = (float)ww / PIC_W < (float)wh / PIC_H ? (float)ww / PIC_W : (float)wh / PIC_H;
        int ox = (int)((ww - PIC_W * s) / 2), oy = (int)((wh - PIC_H * s) / 2);
        real_WarpMouseInWindow(win, ox + (int)(x * s), oy + (int)(y * s));
        real_ShowCursor(SDL_DISABLE);
    }
}

// SDL's events to host_input: keys as USB HID usages (SDL's scancodes are
// the HID usage codes, the modifiers 0xE0..0xE7 included), F12 a
// screenshot; motion in picture coordinates (and SDL's deltas for the
// relative mode); buttons as the USB mouse's bits (0 left, 1 right, 2
// middle).
void win_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            host_input(IN_QUIT, 0, 0, 0, 0, 0, 0);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) host_input(IN_RELEASE_ALL, 0, 0, 0, 0, 0, 0);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int sc = e.key.keysym.scancode, down = e.type == SDL_KEYDOWN;
            if (e.key.repeat) break;   // (the USB keyboard reports a held key once)
            if (sc == SDL_SCANCODE_F12) {
                if (down) host_input(IN_SHOT, 0, 0, 0, 0, 0, 0);
                break;
            }
            if ((sc >= 0x04 && sc <= 0x65) || (sc >= 0xE0 && sc <= 0xE7)) host_input(IN_KEY, sc, down, 0, 0, 0, 0);
            break;
        }
        case SDL_MOUSEMOTION:
            host_input(IN_MOTION, 0, 0, e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1
                    : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b >= 0) host_input(IN_BUTTON, b, e.type == SDL_MOUSEBUTTONDOWN, e.button.x, e.button.y, 0, 0);
            break;
        }
        }
    }
}

// Tests (host.c's script ops wkey, wdown, wup, wmove, wclick, wquit): events pushed into SDL's
// queue, as SDL would make them, to go the window's way.
void win_push_key(int scancode, int down) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.windowID = SDL_GetWindowID(win);
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    e.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)scancode);
    SDL_PushEvent(&e);
}

static int push_x, push_y;

void win_push_motion(int x, int y) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = SDL_GetWindowID(win);
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - push_x;
    e.motion.yrel = y - push_y;
    push_x = x;
    push_y = y;
    SDL_PushEvent(&e);
}

void win_push_button(int b, int down) {   // b: 0 left, 1 right, 2 middle
    static const Uint8 sdl_b[3] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.windowID = SDL_GetWindowID(win);
    e.button.button = sdl_b[b];
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = push_x;
    e.button.y = push_y;
    SDL_PushEvent(&e);
}

void win_push_quit(void) {   // the window closed
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}
