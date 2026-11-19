// System Shock on the m9b SoC: sound, in place of MacSrc/SDLSound.c (the
// snd_* functions over SDL_mixer), the SDL audio stream cutscenes and
// audio logs feed, and MacSrc/Xmi.c (music: pre-rendered sequences streamed
// from the SD card, see Music below).
//
// The hardware plays 48 kHz 16-bit stereo from two 1 KB halves (256
// frames) and raises an interrupt as it moves on from one; the interrupt
// (the trap handler -> sound_irq()) mixes the next half at 24 kHz, each
// result held for 2 frames (as the Tyrian port).
//
// Effects: the game's sounds are VOC files (DIGIFX.RES: 8-bit unsigned
// mono, mostly 11 or 22 kHz); snd_sample_play() decodes one into a signed
// copy, and one of 12 channels steps through it at its rate, with the
// game's volume (0..100) and pan (1 left .. 127 right) as Mix_SetPanning
// did, looping if asked.
//
// Speech (cutscenes, audio logs): SDL_AudioStreamPut() queues blocks of the
// whole sound the game holds in memory until it frees the stream, so the
// stream keeps pointers to them, not copies.
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lg.h"
#include "lgsndx.h"
#include "MacTune.h"
#include "musicai.h"
#include "res.h"
#include "Xmi.h"
#include "SDL.h"
#include "m9b_hw.h"
#include "m9b_map.h"

#define IRQ_PENDING   (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t*)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL    (*(volatile uint32_t*)(M9B_CTRL + 0xCu))
#define AUDIO_PTR     ((volatile uint32_t*)(M9B_CTRL + 0x10u))   // [0] half A, [1] half B
#define IRQ_AUDIO_A   (1u << 1)
#define IRQ_AUDIO_B   (1u << 2)

#define FRAMES        256u      // per half
#define HOLD          2u        // output frames per mixed sample
#define MIX_RATE      24000u
#define NMIX          (FRAMES / HOLD)

uint32_t sound_cycles, sound_mixes;   // time in sound_irq (clock ticks), halves mixed

static uint32_t audio_buf[2][FRAMES] __attribute__((aligned(256)));
static uint32_t silence[FRAMES] __attribute__((aligned(256)));

// -------------------------------------------------------------------------
// Effect channels
// -------------------------------------------------------------------------
static struct channel {
    int8_t *data;          // NULL: idle (owned: freed when the channel ends)
    uint32_t len, pos;     // samples; pos 16.16
    uint32_t step;         // 16.16, rate / MIX_RATE
    int32_t vol_l, vol_r;  // Q8
    int loops;             // more times through after this one; -1 forever
} channels[SND_MAX_SAMPLES];

static snd_digi_parms digi_parms_by_channel[SND_MAX_SAMPLES];

// -------------------------------------------------------------------------
// The audio stream (speech)
// -------------------------------------------------------------------------
struct SDL_AudioStream {
    const uint8_t **blocks;   // queued blocks (the game's memory)
    int *lens;
    int nblocks, cap;
    int head;                 // block being played
    uint32_t pos;             // 16.16 in blocks[head]
    uint32_t step;
};

SDL_AudioStream *cutscene_audiostream;   // (cutsloop.c's, also audiolog.c's)
static volatile int stream_paused = 1;

SDL_AudioStream *SDL_NewAudioStream(Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format,
                                    Uint8 dst_channels, int dst_rate) {
    (void)src_format; (void)src_channels; (void)dst_format; (void)dst_channels; (void)dst_rate;
    SDL_AudioStream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->step = (uint32_t)(((uint64_t)src_rate << 16) / MIX_RATE);
    return s;
}

int SDL_AudioStreamPut(SDL_AudioStream *s, const void *buf, int len) {
    if (!s || len <= 0) return 0;
    if (s->nblocks == s->cap) {
        int cap = s->cap ? 2 * s->cap : 64;
        const uint8_t **b = malloc(sizeof(*b) * cap);
        int *l = malloc(sizeof(*l) * cap);
        if (!b || !l) { free(b); free(l); return -1; }
        uint32_t m = irq_off();
        if (s->nblocks) {
            memcpy(b, s->blocks, sizeof(*b) * s->nblocks);
            memcpy(l, s->lens, sizeof(*l) * s->nblocks);
        }
        free(s->blocks);
        free(s->lens);
        s->blocks = b;
        s->lens = l;
        s->cap = cap;
        irq_restore(m);
    }
    uint32_t m = irq_off();
    s->blocks[s->nblocks] = buf;
    s->lens[s->nblocks] = len;
    s->nblocks++;
    irq_restore(m);
    return 0;
}

int SDL_AudioStreamAvailable(SDL_AudioStream *s) {
    if (!s) return 0;
    uint32_t m = irq_off();
    int64_t left = 0;
    for (int i = s->head; i < s->nblocks; i++) left += s->lens[i];
    if (s->head < s->nblocks) left -= s->pos >> 16;
    irq_restore(m);
    return left > 0 ? (int)(left * 4) : 0;   // (as 16-bit stereo, roughly)
}

int SDL_AudioStreamGet(SDL_AudioStream *s, void *buf, int len) {
    (void)s; (void)buf; (void)len;
    return 0;
}

void SDL_AudioStreamClear(SDL_AudioStream *s) {
    if (!s) return;
    uint32_t m = irq_off();
    s->nblocks = s->head = 0;
    s->pos = 0;
    irq_restore(m);
}

void SDL_FreeAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    uint32_t m = irq_off();
    if (s == cutscene_audiostream) cutscene_audiostream = NULL;
    irq_restore(m);
    free(s->blocks);
    free(s->lens);
    free(s);
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {
    (void)dev;
    stream_paused = pause_on;
}

// -------------------------------------------------------------------------
// The mixer (interrupt)
// -------------------------------------------------------------------------
static inline int32_t clamp16(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static int mix_music(int32_t *mus);   // below, with the rest of the music

static void mix_half(uint32_t half) {
    int32_t acc_l[NMIX], acc_r[NMIX];
    int any = 0;

    SDL_AudioStream *s = cutscene_audiostream;
    if (s && !stream_paused && s->head < s->nblocks) {
        uint32_t i = 0, pos = s->pos;
        while (i < NMIX && s->head < s->nblocks) {
            const uint8_t *d = s->blocks[s->head];
            uint32_t end = (uint32_t)s->lens[s->head] << 16;
            for (; i < NMIX && pos < end; i++, pos += s->step) acc_l[i] = acc_r[i] = ((int32_t)d[pos >> 16] - 128) << 8;
            if (pos >= end) {
                pos -= end;
                s->head++;
            }
        }
        for (; i < NMIX; i++) acc_l[i] = acc_r[i] = 0;
        s->pos = pos;
        any = 1;
    }

    for (int c = 0; c < SND_MAX_SAMPLES; c++) {
        struct channel *ch = &channels[c];
        if (!ch->data) continue;
        if (!any) {
            memset(acc_l, 0, sizeof(acc_l));
            memset(acc_r, 0, sizeof(acc_r));
            any = 1;
        }
        const int8_t *d = ch->data;
        uint32_t pos = ch->pos, end = ch->len << 16, step = ch->step;
        int32_t vl = ch->vol_l, vr = ch->vol_r;
        for (uint32_t i = 0; i < NMIX;) {
            for (; i < NMIX && pos < end; i++, pos += step) {
                int32_t v = d[pos >> 16];
                acc_l[i] += v * vl;
                acc_r[i] += v * vr;
            }
            if (pos < end) break;
            if (ch->loops == 0) {
                ch->data = NULL;   // freed by the main loop (m9b_sound_poll)
                break;
            }
            if (ch->loops > 0) ch->loops--;
            pos -= end;
        }
        ch->pos = pos;
    }

    int32_t mus[NMIX];
    if (mix_music(mus)) {
        if (!any) {
            memcpy(acc_l, mus, sizeof(acc_l));
            memcpy(acc_r, mus, sizeof(acc_r));
        } else {
            for (uint32_t i = 0; i < NMIX; i++) {
                acc_l[i] += mus[i];
                acc_r[i] += mus[i];
            }
        }
        any = 1;
    }
    if (!any) {
        AUDIO_PTR[half] = (uint32_t)silence - PSRAM_BASE;
        return;
    }
    uint32_t *out = audio_buf[half];
    for (uint32_t i = 0; i < NMIX; i++) {
        uint32_t v = (uint16_t)clamp16(acc_l[i]) | (uint32_t)(uint16_t)clamp16(acc_r[i]) << 16;
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    cache_clean(out, sizeof(audio_buf[0]));
    AUDIO_PTR[half] = (uint32_t)out - PSRAM_BASE;
    sound_mixes++;
}

void sound_irq(uint32_t pending) {   // from the trap handler
    if (!(pending & (IRQ_AUDIO_A | IRQ_AUDIO_B))) return;
    extern void m9b_mouse_sample(void);   // m9b_events.c: the mouse buttons, timed
    m9b_mouse_sample();
    uint32_t t0 = CLOCK_CNT;
    if (pending & IRQ_AUDIO_A) mix_half(0);
    if (pending & IRQ_AUDIO_B) mix_half(1);
    sound_cycles += CLOCK_CNT - t0;
}

// Samples whose channel finished in the interrupt, to free here.
static int8_t *owned[SND_MAX_SAMPLES];

static void music_poll(void);

void m9b_sound_poll(void) {
    music_poll();
    for (int c = 0; c < SND_MAX_SAMPLES; c++)
        if (owned[c] && !channels[c].data) {
            free(owned[c]);
            owned[c] = NULL;
        }
}

// -------------------------------------------------------------------------
// SDLSound.c's interface
// -------------------------------------------------------------------------
int snd_start_digital(void) {
    memset(silence, 0, sizeof(silence));
    cache_clean(silence, sizeof(silence));
    uint32_t m = irq_off();
    AUDIO_PTR[0] = (uint32_t)silence - PSRAM_BASE;
    AUDIO_PTR[1] = (uint32_t)silence - PSRAM_BASE;
    IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
    IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    AUDIO_CTRL = 1u;
    irq_restore(m);
    return OK;
}

// A VOC file's sound: 8-bit unsigned mono blocks (type 1, then type 2
// continuations), to a signed copy. Returns the sample count, 0 if none.
static uint32_t voc_decode(const uint8_t *voc, int len, int8_t **out, uint32_t *rate) {
    if (len < 26 || memcmp(voc, "Creative Voice File\x1A", 20) != 0) return 0;
    uint32_t p = voc[20] | voc[21] << 8, total = 0;
    // Pass 1: size and rate; pass 2: copy.
    for (int pass = 0; pass < 2; pass++) {
        uint32_t q = p, n = 0;
        while (q + 4 <= (uint32_t)len && voc[q] != 0) {
            uint32_t type = voc[q], size = voc[q + 1] | voc[q + 2] << 8 | (uint32_t)voc[q + 3] << 16;
            const uint8_t *d = voc + q + 4;
            if (q + 4 + size > (uint32_t)len) size = (uint32_t)len - q - 4;
            if (type == 1 && size >= 2) {
                if (pass == 0) *rate = 1000000u / (256u - d[0]);
                if (d[1] == 0) {   // 8-bit PCM
                    if (pass) for (uint32_t i = 0; i < size - 2; i++) (*out)[n + i] = (int8_t)(d[2 + i] ^ 0x80u);
                    n += size - 2;
                }
            } else if (type == 2) {
                if (pass) for (uint32_t i = 0; i < size; i++) (*out)[n + i] = (int8_t)(d[i] ^ 0x80u);
                n += size;
            }
            q += 4 + size;
        }
        if (pass == 0) {
            total = n;
            if (!n || !(*out = ResMalloc(n))) return 0;   // (paging out resources if need be)
        }
    }
    return total;
}

static void set_volume(int c, const snd_digi_parms *sdp) {
    // vol 0..100 (Mix_Volume(vol * 128 / 100)), pan 1..127 (Mix_SetPanning(254 - 2 pan, 2 pan))
    int32_t vol = sdp->vol > 100 ? 100 : sdp->vol;
    int32_t right = 2 * sdp->pan;
    if (right > 254) right = 254;
    int32_t left = 254 - right;
    // Q8 per side: full volume, centred, is about 0.5 per side (as SDL_mixer's panning).
    channels[c].vol_l = vol * left * 256 / (100 * 255);
    channels[c].vol_r = vol * right * 256 / (100 * 255);
}

int snd_sample_play(int snd_ref, int len, uchar *smp, struct snd_digi_parms *dprm) {
    (void)snd_ref;
    m9b_sound_poll();
    int c;
    for (c = 0; c < SND_MAX_SAMPLES; c++)
        if (!channels[c].data && !owned[c]) break;
    // (No sample: SND_PERROR, not ERR_NOEFFECT, which is 6, a channel to the caller.)
    if (c == SND_MAX_SAMPLES) return SND_PERROR;
    int8_t *data = NULL;
    uint32_t rate = 11025;
    uint32_t n = voc_decode(smp, len, &data, &rate);
    if (!n) {
        free(data);
        return SND_PERROR;
    }
    digi_parms_by_channel[c] = *dprm;
    struct channel ch = {0};
    ch.data = data;
    ch.len = n;
    ch.step = (uint32_t)(((uint64_t)rate << 16) / MIX_RATE);
    ch.loops = dprm->loops > 0 ? dprm->loops - 1 : -1;
    uint32_t m = irq_off();
    channels[c] = ch;
    set_volume(c, &digi_parms_by_channel[c]);
    owned[c] = data;
    irq_restore(m);
    return c;
}

void snd_end_sample(int hnd_id) {
    if (hnd_id < 0 || hnd_id >= SND_MAX_SAMPLES) return;
    uint32_t m = irq_off();
    channels[hnd_id].data = NULL;
    irq_restore(m);
    m9b_sound_poll();
}

bool snd_sample_playing(int hnd_id) {
    return hnd_id >= 0 && hnd_id < SND_MAX_SAMPLES && channels[hnd_id].data != NULL;
}

snd_digi_parms *snd_sample_parms(int hnd_id) { return &digi_parms_by_channel[hnd_id]; }

void snd_kill_all_samples(void) {
    for (int c = 0; c < SND_MAX_SAMPLES; c++) snd_end_sample(c);
    if (cutscene_audiostream) SDL_AudioStreamClear(cutscene_audiostream);
}

void snd_sample_reload_parms(snd_digi_parms *sdp) {
    if (sdp < digi_parms_by_channel || sdp >= digi_parms_by_channel + SND_MAX_SAMPLES) return;
    int c = (int)(sdp - digi_parms_by_channel);
    if (!channels[c].data) return;
    uint32_t m = irq_off();
    set_volume(c, sdp);
    irq_restore(m);
}

void snd_startup(void) {}
int snd_stop_digital(void) { return 1; }

// -------------------------------------------------------------------------
// Music. The game's music is XMI sequences ("pieces") in themes
// (SOUND/SBLASTER/THM*.XMI, TITLOOP, ENDLOOP, ...): the music AI (musicai.c,
// mlimbs.c) starts them one at a time on up to 8 threads, the main piece on
// thread 0 and shorter layers repeated on the others, and polls IsPlaying()
// to queue the next. Each sequence is rendered on a PC
// (tools/make_music.sh) to SSHOCK/MUSIC/<THEME>_<N>.WAV, 24 kHz 16-bit mono
// with one gain per theme, so a thread here streams one file: the main loop
// (m9b_sound_poll, StartTrack) keeps each playing thread's 16 KB ring
// (0.34 s; 128 KB in all) filled from the card, and the audio interrupt
// adds the threads to the mix. A sequence without a file (the silent ones
// aren't rendered) ends at once. Volume: curr_vol_lev, 0..100 (the audio
// logs duck it), 100 playing the render as it is.
// -------------------------------------------------------------------------
#define MUS_RATE   24000u
#define MUS_RING   (8u * 1024u)    // samples per thread
#define MUS_CHUNK  (4u * 1024u)    // samples per read (8 KB)

struct mthread {
    int16_t ring[MUS_RING];
    volatile uint32_t r, w;        // read by the interrupt, written by the main loop
    volatile int playing;
    int open, fd, eof;             // open: fd is a file (zero at start: no init call needed)
    uint32_t end;                  // file offset of the samples' end
};
static struct mthread mthreads[NUM_THREADS] __attribute__((aligned(64)));
static char theme[16];
static int32_t mus_gain = 256;     // Q8
unsigned int NumTracks;

// The interrupt's part: the playing threads' sum into mus (mono). Returns
// whether any played.
static int mix_music(int32_t *mus) {
    int played = 0;
    for (int t = 0; t < NUM_THREADS; t++) {
        struct mthread *m = &mthreads[t];
        if (!m->playing) continue;
        if (!played) memset(mus, 0, NMIX * sizeof(*mus));
        played = 1;
        uint32_t r = m->r, w = m->w;
        for (uint32_t i = 0; i < NMIX && r != w; i++) mus[i] += (m->ring[r++ & (MUS_RING - 1u)] * mus_gain) >> 8;
        m->r = r;
    }
    return played;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// Opens a WAV file on thread m: 0 if it is 24 kHz 16-bit mono PCM, positioned at its samples.
static int open_wav(struct mthread *m, const char *path) {
    uint8_t h[16];
    uint32_t pos = 12;
    int fmt_ok = 0;
    m->fd = open(path, O_RDONLY);
    if (m->fd < 0) return -1;
    m->open = 1;
    if (read(m->fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    for (;;) {   // chunks: "fmt " then "data" (others skipped)
        if (lseek(m->fd, pos, SEEK_SET) != (off_t)pos || read(m->fd, h, 8) != 8) return -1;
        uint32_t size = le32(h + 4);
        if (!memcmp(h, "fmt ", 4)) {
            if (read(m->fd, h, 16) != 16) return -1;
            fmt_ok = h[0] == 1 && h[1] == 0 && h[2] == 1 && le32(h + 4) == MUS_RATE && h[14] == 16;
        } else if (!memcmp(h, "data", 4)) {
            if (!fmt_ok) return -1;
            m->end = pos + 8u + (size & ~1u);
            return 0;   // positioned after the header: the samples
        }
        pos += 8u + ((size + 1u) & ~1u);
    }
}

// Tops up a thread's ring with up to max_chunks reads.
static void fill(struct mthread *m, int max_chunks) {
    while (m->open && !m->eof && max_chunks-- > 0) {
        uint32_t w = m->w, space = MUS_RING - (w - m->r);
        uint32_t at = w & (MUS_RING - 1u), n = MUS_RING - at;
        if (n > MUS_CHUNK) n = MUS_CHUNK;
        if (space < n) return;
        uint32_t left = (m->end - (uint32_t)lseek(m->fd, 0, SEEK_CUR)) / 2u;
        if (n > left) n = left;
        ssize_t got = n ? read(m->fd, &m->ring[at], n * 2u) : 0;
        if (got <= 0) { m->eof = 1; return; }
        m->w = w + (uint32_t)got / 2u;
    }
}

static void music_poll(void) {
    for (int t = 0; t < NUM_THREADS; t++) {
        struct mthread *m = &mthreads[t];
        if (m->playing) fill(m, 2);
        if (m->playing && m->eof && m->r == m->w) StopTrack(t);   // done: free the file
    }
}

// A theme's music AI tables: SOUND/<THEME>.BIN of the DOS version (copied to
// SSHOCK/RES/SOUND/ by tools/put_data.sh), as upstream's Xmi.c reads them;
// the loops (TITLOOP, ...) have none.
int MacTuneLoadTheme(char *theme_base, int themeID) {
    extern uchar track_table[NUM_SCORES][SUPERCHUNKS_PER_SCORE];
    extern uchar transition_table[NUM_TRANSITIONS];
    extern uchar layering_table[NUM_LAYERS][MAX_KEYS];
    extern uchar key_table[NUM_LAYERABLE_SUPERCHUNKS][KEY_BAR_RESOLUTION];
    (void)themeID;
    StopTheMusic();
    unsigned i;
    for (i = 0; theme_base[i] && i < sizeof(theme) - 1; i++) theme[i] = (char)toupper((unsigned char)theme_base[i]);
    theme[i] = 0;
    char path[40];
    snprintf(path, sizeof(path), "res/sound/%s.bin", theme_base);
    FILE *f = fopen_caseless(path, "rb");
    if (f) {
        size_t n = fread(track_table, sizeof(track_table), 1, f);
        n += fread(transition_table, sizeof(transition_table), 1, f);
        n += fread(layering_table, sizeof(layering_table), 1, f);
        n += fread(key_table, sizeof(key_table), 1, f);
        fclose(f);
        if (n != 4) WARN("%s: short", path);
    }
    NumTracks = 64;   // any piece the AI asks for; those without a file are silent
    return OK;
}

void MacTuneKillCurrentTheme(void) { StopTheMusic(); }

void StartTrack(int i, unsigned int track) {
    if (i < 0 || i >= NUM_THREADS) return;
    struct mthread *m = &mthreads[i];
    char path[40];
    StopTrack(i);
    UpdateVolumeXMI();
    snprintf(path, sizeof(path), "MUSIC/%s_%u.WAV", theme, track);   // SSHOCK/ is added by open()
    if (open_wav(m, path) < 0) {
        StopTrack(i);
        return;
    }
    m->eof = 0;
    m->r = m->w = 0;
    fill(m, 2);
    m->playing = 1;
}

void StopTrack(int i) {
    if (i < 0 || i >= NUM_THREADS) return;
    struct mthread *m = &mthreads[i];
    m->playing = 0;
    if (m->open) close(m->fd);
    m->open = 0;
}

void StopTheMusic(void) {
    for (int t = 0; t < NUM_THREADS; t++) StopTrack(t);
}

int IsPlaying(int i) {
    if (i < 0 || i >= NUM_THREADS) return 0;
    struct mthread *m = &mthreads[i];
    return m->playing && !(m->eof && m->r == m->w);
}

void InitReadXMI(void) {}
void InitDecXMI(void) {}
void ReloadDecXMI(void) {}
void ShutdownReadXMI(void) { StopTheMusic(); }
unsigned int GetOutputCountXMI(void) { return 0; }
void GetOutputNameXMI(const unsigned int outputIndex, char *buffer, const unsigned int bufferSize) {
    (void)outputIndex;
    if (bufferSize) buffer[0] = 0;
}
void UpdateVolumeXMI(void) {
    extern uchar curr_vol_lev;
    mus_gain = (curr_vol_lev > 100 ? 100 : curr_vol_lev) * 256 / 100;
}
