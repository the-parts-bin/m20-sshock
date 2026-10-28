// System Shock on the m9b SoC: files and the few POSIX functions the game
// uses beyond picolibc and ../../core/runtime/syscalls.c (files through FatFs).
//
// Everything is under SSHOCK/ on the SD card. The game opens relative
// paths ("res/data/citmat.res", "prefs.txt", the saves), and the linker
// sends open(), unlink() and rename() (fopen, remove, ... included) through
// the __wrap_ functions here, which put SSHOCK/ in front of them.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "ff.h"

#define DATA_DIR "SSHOCK/"

extern uint32_t SDL_GetTicks(void);   // m9b_shock.c

static const char *data_path(const char *path, char *buf, size_t n) {
    if (path[0] == '/') return path;
    if (path[0] == '.' && path[1] == '/') path += 2;
    snprintf(buf, n, DATA_DIR "%s", path);
    return buf;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    char buf[256];
    return __real_open(data_path(path, buf, sizeof(buf)), flags);
}

int __real_unlink(const char *path);
int __wrap_unlink(const char *path) {
    char buf[256];
    return __real_unlink(data_path(path, buf, sizeof(buf)));
}

int __real_rename(const char *a, const char *b);
int __wrap_rename(const char *a, const char *b) {
    char ba[256], bb[256];
    const char *pb = data_path(b, bb, sizeof(bb));
    f_unlink(pb);   // POSIX replaces the target; FatFs doesn't
    return __real_rename(data_path(a, ba, sizeof(ba)), pb);
}

int access(const char *path, int mode) {
    (void)mode;
    char buf[256];
    FILINFO fi;
    return f_stat(data_path(path, buf, sizeof(buf)), &fi) == FR_OK ? 0 : -1;
}

int isatty(int fd) { return fd < 3; }

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint32_t ms = SDL_GetTicks();
    tv->tv_sec = ms / 1000u;
    tv->tv_usec = (ms % 1000u) * 1000u;
    return 0;
}

// No console input (the UART is the keyboard, see m9b_events.c).
static int no_input(FILE *f) { (void)f; return EOF; }
static FILE input = FDEV_SETUP_STREAM(NULL, no_input, NULL, _FDEV_SETUP_READ);
FILE *const stdin = &input;
