// System Shock on the m20: the span engine for the main view's walls,
// floors and ceilings (port/m20_engine.c) and what it needs from the video
// (port/m9b_shock.c).
#ifndef M20_ENGINE_H
#define M20_ENGINE_H

#include <stdint.h>
#include "fix.h"

#define M20_HOLE 255   // a canvas pixel the engine drew

struct grs_bitmap;
extern int m20_engine_enable;   // Ctrl-K on the terminal
extern int m20_engine_on;       // in the main view's rendering
extern int m20_engine_frame;    // the engine drew in this frame
extern unsigned int m20_engine_faces, m20_engine_spans, m20_engine_wait_cycles;

void m20_engine_wait(void);
void m20_tex_loaded(int slot, uint8_t *p, uint32_t n, const uint32_t *pal);

// m9b_shock.c: the view canvas's (0, 0) in the frame the engine draws into
// (RGB565; stride in pixels), at the frame's first engine polygon; the
// palette as RGB888 with a count of its changes
uint16_t *m20_video_engine_target(uint32_t *stride, int w, int h);
const uint32_t *m20_video_pal888(uint32_t *generation);

#endif
