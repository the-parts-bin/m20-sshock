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

#include "afile.h"
#include "res.h"
#include "res_.h"

#ifdef M9B
#include <fcntl.h>
#include <unistd.h>

// M9B: a movie (megabytes) read in place from its resource file, if it is
// stored plainly, instead of copied into memory: the video and the audio
// each get their own file descriptor, so that both read forward.
static MFILE *open_in_place(Id id, int fd) {
    MFILE *mf = (MFILE *)malloc(sizeof(MFILE));
    if (!mf) return NULL;
    mf->p = NULL;
    mf->size = RESDESC(id)->fsize;
    mf->pos = 0;
    mf->fd = fd;
    mf->base = RESDESC(id)->offset;
    mf->fdpos = -1;
    return mf;
}

static int32_t prepare_in_place(Id id, Afile *afile) {
    const char *name;
    if (!ResInUse(id) || (RESDESC2(id)->flags & (RDF_LZW | RDF_COMPOUND)) ||
        !(name = ResFileName(RESDESC(id)->filenum)))
        return -1;
    int fd = open(name, O_RDONLY), afd = open(name, O_RDONLY);
    MFILE *mf = fd >= 0 ? open_in_place(id, fd) : NULL;
    MFILE *amf = afd >= 0 ? open_in_place(id, afd) : NULL;
    if (!mf || !amf) {
        free(mf);
        free(amf);
        if (fd >= 0) close(fd);
        if (afd >= 0) close(afd);
        return -1;
    }
    int32_t error = AfileOpen(afile, mf, AFILE_MOV);
    if (error < 0) {
        close(fd);
        close(afd);
        free(mf);
        free(amf);
        return error;
    }
    afile->amf = amf;
    return error;
}
#endif

int32_t AfilePrepareRes(Id id, Afile *afile) {

#ifdef M9B
    if (prepare_in_place(id, afile) == 0)
        return 0;
#endif

    // Grab the raw data and let the library deal with it.
    uint8_t *ptr = ResLock(id);
    int size = ResSize(id);
    MFILE *mf;

    mf = (MFILE *)malloc(sizeof(MFILE));
    mf->p = (unsigned char *)malloc(size);
    memcpy(mf->p, ptr, size);
    mf->size = size;
    mf->pos = 0;

    ResUnlock(id);

    int32_t error = AfileOpen(afile, mf, AFILE_MOV);

    return error;
}
