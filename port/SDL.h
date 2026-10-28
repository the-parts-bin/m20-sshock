// An SDL2 stand-in for Shockolate on the m9b SoC: the types, constants and
// functions the game and library code still use once the platform files
// (Shock.c, ShockBitmap.c, SDLSound.c, Xmi.c, sdl_events.c) are replaced by
// port/. Implemented in m9b_shock.c (time, mouse mode, paths) and
// m9b_sound.c (the audio stream that cutscenes and audio logs feed).
#ifndef M9B_SDL_H
#define M9B_SDL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

// Time (milliseconds since start-up)
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

// One thread: atomics and mutexes are plain variables and no-ops.
typedef struct { int value; } SDL_atomic_t;
static inline int SDL_AtomicGet(SDL_atomic_t *a) { return a->value; }
static inline int SDL_AtomicSet(SDL_atomic_t *a, int v) { int o = a->value; a->value = v; return o; }
typedef struct SDL_mutex SDL_mutex;
static inline SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)1; }
static inline void SDL_DestroyMutex(SDL_mutex *m) { (void)m; }
static inline int SDL_LockMutex(SDL_mutex *m) { (void)m; return 0; }
static inline int SDL_UnlockMutex(SDL_mutex *m) { (void)m; return 0; }

#define SDL_memset memset
#define SDL_free free

// Video: an 8 bpp surface is all the game touches directly.
typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
typedef struct SDL_Surface {
    int w, h, pitch;
    void *pixels;
} SDL_Surface;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
void SDL_GetWindowSize(SDL_Window *w, int *width, int *height);

// Mouse
#define SDL_BUTTON(x) (1u << ((x) - 1))
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_LMASK SDL_BUTTON(SDL_BUTTON_LEFT)
#define SDL_BUTTON_RMASK SDL_BUTTON(SDL_BUTTON_RIGHT)
#define SDL_QUERY (-1)
#define SDL_DISABLE 0
#define SDL_ENABLE 1
Uint32 SDL_GetMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(SDL_bool on);
SDL_bool SDL_GetRelativeMouseMode(void);
void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y);
int SDL_ShowCursor(int toggle);

// Keyboard modifiers
#define KMOD_SHIFT 0x0003
#define KMOD_CTRL 0x00C0
#define KMOD_ALT 0x0300
int SDL_GetModState(void);

// Audio: cutscenes and audio logs push 8-bit mono sound through a stream.
#define AUDIO_U8 0x0008
#define AUDIO_S16SYS 0x8010
typedef Uint32 SDL_AudioDeviceID;
typedef struct SDL_AudioStream SDL_AudioStream;
SDL_AudioStream *SDL_NewAudioStream(Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format,
                                    Uint8 dst_channels, int dst_rate);
int SDL_AudioStreamPut(SDL_AudioStream *s, const void *buf, int len);
int SDL_AudioStreamGet(SDL_AudioStream *s, void *buf, int len);
int SDL_AudioStreamAvailable(SDL_AudioStream *s);
void SDL_AudioStreamClear(SDL_AudioStream *s);
void SDL_FreeAudioStream(SDL_AudioStream *s);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);

// Files: everything lives under one directory on the card (see m9b_shock.c).
char *SDL_GetPrefPath(const char *org, const char *app);
char *SDL_GetBasePath(void);

#ifdef __cplusplus
}
#endif

#endif
