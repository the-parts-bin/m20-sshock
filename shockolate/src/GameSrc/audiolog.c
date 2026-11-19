/*

Copyright (C) 2015-2018 Night Dive Studios, LLC.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
/*
 * $Source: r:/prj/cit/src/RCS/audiolog.c $
 * $Revision: 1.17 $
 * $Author: dc $
 * $Date: 1994/11/19 20:35:27 $
 */
//	Mac version by Ken Cobb,  2/9/95

#include <stdio.h>
#include <SDL.h>

#include "MacTune.h"
#include "afile.h"
#ifdef M9B
#include "res.h"   // (ResMalloc)
#endif
#include "movie.h"
#include "audiolog.h"
#include "map.h"
#include "tools.h"
#include "musicai.h"
#include "mainloop.h"
#include "bark.h"
#include "miscqvar.h"

#define AUDIOLOG_BASE_ID 2741
#define AUDIOLOG_BARK_BASE_ID 3100

#define ALOG_MUSIC_DUCK 0.7

extern SDL_AudioStream *cutscene_audiostream; // see cutsloop.c

static uint8_t *audiolog_audiobuffer = NULL;
static uint8_t *audiolog_audiobuffer_pos = NULL;
static int audiolog_audiobuffer_size; // in blocks of MOVIE_DEFAULT_BLOCKLEN

#ifdef M9B
// M9B: the log's audio read as it plays, as the cutscenes' (cutsloop.c).
#define AUDIO_RING 8
extern void m9b_feed_audio(SDL_AudioStream *stream, Afile *af, uint8_t *ring, int *blocks_left, int *blocks_put,
                           int volume);
static Afile *alog_movie;
static int alog_blocks;
#endif

int curr_alog = -1;
int alog_fn = -1;
uchar audiolog_setting = 1;
char secret_pending_hack;

char *bark_files[] = {"res/data/citbark.res", "res/data/frnbark.res", "res/data/gerbark.res"};
char *alog_files[] = {"res/data/citalog.res", "res/data/frnalog.res", "res/data/geralog.res"};

extern uchar curr_vol_lev;
extern uchar curr_alog_vol;
extern char which_lang;

extern SDL_AudioDeviceID device;

errtype audiolog_init(void) { return OK; }

errtype audiolog_play(int email_id) {
    int new_alog_fn;
    Afile *palog;

    if (!sfx_on || !audiolog_setting)
        return ERR_NOEFFECT;

    // KLC - Big-time hack to prevent bark #389 from trying to play twice (and thus skipping).
    if (email_id == 389 && curr_alog == email_id)
        return ERR_NOEFFECT;

    // Stop any currently playing alogs.
    audiolog_stop();

    // woo hoo, what a hack!
    // this is for the player's log-to-self which has no audiolog
    if (email_id == 0x44)
        return ERR_NOEFFECT;

    begin_wait();

    // Open up the appropriate sound-only movie file.
    if (email_id > (AUDIOLOG_BARK_BASE_ID - AUDIOLOG_BASE_ID))
        new_alog_fn = ResOpenFile(bark_files[which_lang]);
    else
        new_alog_fn = ResOpenFile(alog_files[which_lang]);
#ifdef M9B
    // The logs and barks can be left off the card (tools/put_data.sh
    // --no-logs): then the email shows its text, without the audio.
    if (new_alog_fn < 0) {
        end_wait();
        return ERR_FOPEN;
    }
#endif

    // Make sure this is a thing we have an audiolog for...
    if (!ResInUse(AUDIOLOG_BASE_ID + email_id)) {
        ResCloseFile(new_alog_fn);
        end_wait();
        return ERR_FREAD;
    }

    alog_fn = new_alog_fn;

    palog = malloc(sizeof(Afile));
    if (AfilePrepareRes(AUDIOLOG_BASE_ID + email_id, palog) < 0) {
        WARN("%s: Cannot open Afile by id $%x", __FUNCTION__, AUDIOLOG_BASE_ID + email_id);
        free(palog);
        return ERR_FREAD;
    }

    audiolog_audiobuffer_size = AfileAudioLength(palog);
#ifdef M9B
    // M9B: the 64 KB ring through ResMalloc (unlocked resources paged out if
    // the heap has no block that big: at 640 x 480 it can be fragmented with
    // plenty free), and checked: AmovReadAudioBlock wrote through NULL.
    audiolog_audiobuffer = (uint8_t *)ResMalloc(AUDIO_RING * MOVIE_DEFAULT_BLOCKLEN);
    if (audiolog_audiobuffer == NULL) {
        WARN("%s: no memory for the audio log's ring", __FUNCTION__);
        AfileFree(palog);
        free(palog);
        end_wait();
        return ERR_FREAD;
    }
    alog_movie = palog;
    alog_blocks = 0;
#else
    audiolog_audiobuffer = (uint8_t *)malloc(audiolog_audiobuffer_size * MOVIE_DEFAULT_BLOCKLEN);
    AfileGetAudio(palog, audiolog_audiobuffer);
#endif

    DEBUG("%s: Playing email", __FUNCTION__);

    SDL_PauseAudioDevice(device, 1);
    SDL_Delay(1);

    cutscene_audiostream = SDL_NewAudioStream(AUDIO_U8, 1, fix_int(palog->a.sampleRate), AUDIO_S16SYS, 2, 48000);

    audiolog_audiobuffer_pos = audiolog_audiobuffer;

    end_wait();

    // bureaucracy
    curr_alog = email_id;

    // Duck the music
    if (music_on) {
        curr_vol_lev = QVAR_TO_VOLUME(QUESTVAR_GET(MUSIC_VOLUME_QVAR));
        curr_vol_lev = curr_vol_lev * ALOG_MUSIC_DUCK;
        MacTuneUpdateVolume();
    }

    return OK;
}

void audiolog_stop(void) {
    if (alog_fn < 0)
        return;

    ResCloseFile(alog_fn);
    alog_fn = -1;

    // Restore music volume
    if (music_on) {
        curr_vol_lev = QVAR_TO_VOLUME(QUESTVAR_GET(MUSIC_VOLUME_QVAR));
        MacTuneUpdateVolume();
    }

    if (cutscene_audiostream != NULL) {
        SDL_PauseAudioDevice(device, 1);
        SDL_Delay(1);

        SDL_FreeAudioStream(cutscene_audiostream);
        cutscene_audiostream = NULL;

        if (audiolog_audiobuffer) {
            free(audiolog_audiobuffer);
            audiolog_audiobuffer = NULL;
        }
    }

#ifdef M9B
    // M9B: the log's movie (upstream left it, and its files, behind) and the ring.
    if (alog_movie) {
        AfileFree(alog_movie);
        free(alog_movie);
        alog_movie = NULL;
    }
    free(audiolog_audiobuffer);
    audiolog_audiobuffer = NULL;
#endif

    curr_alog = -1;

    if (secret_pending_hack) {
        INFO("Game over.");

        secret_pending_hack = 0;

        // Back to the main menu
        _new_mode = SETUP_LOOP;
        chg_set_flg(GL_CHG_LOOP);
    }
}

errtype audiolog_loop_callback(void) {
    if (cutscene_audiostream) {
        SDL_PauseAudioDevice(device, 0);

#ifdef M9B
        m9b_feed_audio(cutscene_audiostream, alog_movie, audiolog_audiobuffer, &audiolog_audiobuffer_size,
                       &alog_blocks, curr_alog_vol * 127 / 100);
#else
        if (audiolog_audiobuffer_size > 0) {
            int i, vol = curr_alog_vol * 127 / 100; // convert from 0-100 to 0-127

            for (i = 0; i < MOVIE_DEFAULT_BLOCKLEN; i++)
                audiolog_audiobuffer_pos[i] = 128 + ((int)audiolog_audiobuffer_pos[i] - 128) * vol / 128;

            SDL_AudioStreamPut(cutscene_audiostream, audiolog_audiobuffer_pos, MOVIE_DEFAULT_BLOCKLEN);
            audiolog_audiobuffer_pos += MOVIE_DEFAULT_BLOCKLEN;
            audiolog_audiobuffer_size--;
        }
#endif

        if (SDL_AudioStreamAvailable(cutscene_audiostream) == 0)
            audiolog_stop();
    }

    return OK;
}

//-------------------------------------------------------------
// if email_id is -1, returns whether or not anything is playing
// if email_id != -1, matches whether or not that specific email_id is playing
//-------------------------------------------------------------
bool audiolog_playing(int email_id) {
    if (email_id == -1)
        return (curr_alog != -1);
    else
        return (curr_alog == email_id);
}

//-------------------------------------------------------------
//  Start playing a bark file.
//-------------------------------------------------------------
errtype audiolog_bark_play(int bark_id) {
    if (global_fullmap->cyber)
        return ERR_NOEFFECT;
    else
        return (audiolog_play(bark_id + (AUDIOLOG_BARK_BASE_ID - AUDIOLOG_BASE_ID)));
}

//-------------------------------------------------------------
//  Stop playing audiolog (in response to a hotkey).
//-------------------------------------------------------------
uchar audiolog_cancel_func(ushort s, uint32_t l, intptr_t v) {
    audiolog_stop();
    return TRUE;
}
