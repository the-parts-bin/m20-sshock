// System Shock on the m20: the span engine (../../core/rtl/span_engine.v)
// draws the 3D view's walls, floors and ceilings, RGB565, straight into the
// frame shown (m9b_shock.c: m20_video_engine_target()), while the CPU draws
// everything else into the game's 8 bpp canvases as before (as Descent's
// port/m20_engine.c, which explains the scheme).
//
// Sharing the frame: under each engine span the 3D view's canvas gets holes
// (M20_HOLE), which the view's copy to the screen keeps (a plain copy), and
// SDLDraw() writes the screen's other pixels over the engine's, through the
// palette. Whichever drew a pixel last wins.
//
// A polygon goes to the engine (draw_tmap_common, 3D/Source/tmap.c) when its
// bitmap is one of the level's textures (load_textures: tmap_static_mem and
// tmap_big_buffer, cleaned from the cache and with their colours moved off
// 224..255 there: the engine leaves those unlit) and the polygon is drawn in
// the main view (m20_engine_on, frmain.c). Its spans come from the scan
// conversion here (Shock's rule: rows and columns from fix_cint of the
// edges, the right and bottom edges left out).
//
// Light: the vertices' intensity picks one of the shading table's 16 rows
// (i >> 16); each row is matched as one brightness factor (least squares
// over the palette), which the engine multiplies each colour by, from a
// small lightmap fitted to the vertices.
#ifdef M20_ENGINE
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "2d.h"
#include "m20_engine.h"

#define SPAN_HW      ((volatile uint32_t *)0x490000u)
#define SPAN_BUSY    (1u << 31)
#define SDRAM_OFF(p) ((uint32_t)(uintptr_t)(p) - 0x800000u)
#define LIT_A        (1u << 30)
#define LIT_B        (2u << 30)
#define CLOCK_CNT    (*(volatile uint32_t *)0x44000Cu)

#ifndef M20_ENGINE_MODE
#define M20_ENGINE_MODE 1
#endif
int m20_engine_enable = M20_ENGINE_MODE;   // Ctrl-K on the terminal (1 all, 2 walls only, 3 the rest, 0 off)
int m20_engine_on;          // in the main view's rendering (frmain.c)
int m20_main_view;          // render_run's fr_rend (render.c)
int m20_engine_frame;       // the engine drew in this frame
unsigned int m20_engine_faces, m20_engine_spans, m20_engine_wait_cycles;   // for the profile report
unsigned int m20_tmap_cycles;   // m20_engine_tmap's time   // m20_engine_tmap's time, and the holes' part of it

static int hw_queued;
static uint16_t *target;    // the view canvas's (0, 0) in the engine's target
static uint32_t target_stride;
static uint32_t cur_pal_gen = ~0u;
static int16_t zdummy[1024] __attribute__((aligned(64)));   // the engine's z writes: one row

// The textures it may use (load_textures)
static const uint8_t *tex_lo[2], *tex_hi[2];

// The lightmaps: a ring, each 64-byte aligned (the engine loads them by lines)
#define LM_RING 32768
static uint8_t lm_ring[LM_RING] __attribute__((aligned(64)));
static uint32_t lm_pos;

static float row_scale[16];   // each shading row's brightness, 128 = as it is
static int rows_ready;

static void cache_clean(const void *p, uint32_t n) {
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}

void m20_engine_wait(void) {
    if (!hw_queued) return;
    uint32_t t = CLOCK_CNT;
    while (SPAN_HW[20] & SPAN_BUSY) {}
    m20_engine_wait_cycles += CLOCK_CNT - t;
    hw_queued = 0;
}

// The level's textures, loaded (load_textures): colours off 224..255 (the
// nearest of 1..223 in the palette, pal: 256 x RGB888), to SDRAM; slot 0
// tmap_static_mem, 1 tmap_big_buffer
void m20_tex_loaded(int slot, uint8_t *p, uint32_t n, const uint32_t *pal) {
    uint8_t remap[256];
    for (int i = 0; i < 256; i++) remap[i] = (uint8_t)i;
    for (int i = 224; i < 256; i++) {
        int r = pal[i] >> 16 & 255, g = pal[i] >> 8 & 255, b = pal[i] & 255;
        int best = 1, bestd = 1 << 30;
        for (int j = 1; j < 224; j++) {
            int dr = r - (int)(pal[j] >> 16 & 255), dg = g - (int)(pal[j] >> 8 & 255), db = b - (int)(pal[j] & 255);
            int d = dr * dr * 3 + dg * dg * 6 + db * db;
            if (d < bestd) { bestd = d; best = j; }
        }
        remap[i] = (uint8_t)best;
    }
    for (uint32_t i = 0; i < n; i++) p[i] = remap[p[i]];
    cache_clean(p, n);
    m20_engine_wait();
    SPAN_HW[0] = LIT_B;                  // a new texture base with lighting off:
    SPAN_HW[1] = SDRAM_OFF(p);           // the engine's texel cache emptied
    tex_lo[slot] = p;
    tex_hi[slot] = p + n;
    rows_ready = 0;
}

// Each shading row as one brightness (least squares over the palette,
// leaving out what the remap saturates)
static void make_rows(const uint32_t *pal) {
    const uint8_t *lt = gr_get_light_tab();
    for (int g = 0; g < 16; g++) {
        float num = 0, den = 0;
        for (int i = 1; i < 255; i++) {
            uint32_t a = pal[i], b = pal[lt[g * 256 + i]];
            if ((b >> 16 & 255) == 255 || (b >> 8 & 255) == 255 || (b & 255) == 255) continue;
            for (int k = 0; k < 24; k += 8) {
                float x = (float)(a >> k & 255), y = (float)(b >> k & 255);
                num += x * y;
                den += x * x;
            }
        }
        row_scale[g] = (den > 0 ? num / den : 1.0f) * 128.0f;
    }
    rows_ready = 1;
}

static float light_scale(fix i) {
    float r = (float)i * (1.0f / 65536.0f);
    if (r < 0) r = 0;
    if (r > 15) r = 15;
    int k = (int)r;
    float f = r - (float)k, s = row_scale[k];
    if (k < 15) s += (row_scale[k + 1] - s) * f;
    return s < 1 ? 1 : s > 255 ? 255 : s;
}

// ---------------------------------------------------------------------
// Fixed point (as ../../quake/port/span_engine.c)
// ---------------------------------------------------------------------
static int cv_w, cv_h;

static int sp_exponent(float o, float dx, float dy) {
    float w = (float)(cv_w - 1), h = (float)(cv_h - 1);
    float m = fabsf(o), c;
    c = fabsf(o + w * dx); if (c > m) m = c;
    c = fabsf(o + h * dy); if (c > m) m = c;
    c = fabsf(o + w * dx + h * dy); if (c > m) m = c;
    if (m < 1e-30f) return 60;
    union { float f; uint32_t u; } b = {m};
    int e = 30 - ((int)(b.u >> 23) - 126);
    if (e > 60) e = 60;
    if (e < 0) e = 0;
    return e;
}

static int32_t sp_fix(float x, int e) {
    union { uint32_t u; float f; } scale = {(uint32_t)(e + 127) << 23};
    float v = x * scale.f;
    return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

static int32_t sp_origin(float o, float dx, float dy, int32_t ix, int32_t iy, int e) {
    int cx = cv_w / 2, cy = cv_h / 2;
    return (int32_t)((uint32_t)sp_fix(o + (float)cx * dx + (float)cy * dy, e)
                     - (uint32_t)cx * (uint32_t)ix - (uint32_t)cy * (uint32_t)iy);
}

static inline int cint(fix x) { return (int)((x + 0xFFFF) >> 16); }   // fix_cint

// ---------------------------------------------------------------------
// A polygon: n vertices (screen x, y in the canvas; u, v 16.16 texels;
// i the light), z their depths. Returns 1 if the engine drew it.
// ---------------------------------------------------------------------
static int engine_tmap(grs_bitmap *bm, int n, grs_vertex *vl, const fix *z, int lit, int wall);
int m20_engine_tmap(grs_bitmap *bm, int n, grs_vertex *vl, const fix *z, int lit, int wall) {
    uint32_t t = CLOCK_CNT;
    int r = engine_tmap(bm, n, vl, z, lit, wall);
    m20_tmap_cycles += CLOCK_CNT - t;
    return r;
}

static int engine_tmap(grs_bitmap *bm, int n, grs_vertex *vl, const fix *z, int lit, int wall) {
    if (!m20_engine_on || !m20_engine_enable) return 0;
    if ((m20_engine_enable == 2 && !wall) || (m20_engine_enable == 3 && wall)) return 0;   // (tests: walls, the rest)
    if (n < 3 || n > 32 || bm->type != BMT_FLAT8 || (bm->flags & BMF_TRANS)) return 0;
    if (bm->row != (1 << bm->wlog) || bm->w != (1 << bm->wlog) || bm->h != (1 << bm->hlog)) return 0;
    if (bm->wlog > 7 || bm->hlog > 7) return 0;
    const uint8_t *bits = bm->bits;
    if (!((bits >= tex_lo[0] && bits < tex_hi[0]) || (bits >= tex_lo[1] && bits < tex_hi[1]))) return 0;
    grs_canvas *cv = grd_canvas;
    if (cv->bm.type != BMT_FLAT8) return 0;
    int wlog = bm->wlog, hlog = bm->hlog;
    float tscale = 1.0f;

    if (!m20_engine_frame) {   // the frame's first: the target, the engine's buffers and palette
        uint32_t stride;
        uint16_t *t = m20_video_engine_target(&stride, cv->bm.w, cv->bm.h);
        if (!t) return 0;
        m20_engine_frame = 1;
        target = t;
        target_stride = stride;
        lm_pos = 0;
        uint32_t gen;
        const uint32_t *pal = m20_video_pal888(&gen);
        if (gen != cur_pal_gen) {
            m20_engine_wait();
            SPAN_HW[24] = 0;
            for (int k = 0; k < 256; k++) SPAN_HW[25] = pal[k];
            cur_pal_gen = gen;   // (the light rows stay as fitted at the level's load: the palette's cycling doesn't move them)
        }
        if (!rows_ready) make_rows(pal);
        SPAN_HW[16] = SDRAM_OFF(target);
        SPAN_HW[17] = target_stride;
        SPAN_HW[18] = SDRAM_OFF(zdummy);
        SPAN_HW[19] = 0;
    }
    cv_w = cv->bm.w;
    cv_h = cv->bm.h;

    float X[32], Y[32], W[32], U[32], V[32];
    float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f, zmin = 1e30f;
    for (int i = 0; i < n; i++) {
        X[i] = (float)vl[i].x * (1.0f / 65536.0f);
        Y[i] = (float)vl[i].y * (1.0f / 65536.0f);
        W[i] = (float)(z[i] < 16 ? 16 : z[i]);
        if (W[i] < zmin) zmin = W[i];
        U[i] = (float)vl[i].u * (tscale / 65536.0f);
        V[i] = (float)vl[i].v * (tscale / 65536.0f);
        if (U[i] < umin) umin = U[i];
        if (U[i] > umax) umax = U[i];
        if (V[i] < vmin) vmin = V[i];
        if (V[i] > vmax) vmax = V[i];
    }
    for (int i = 0; i < n; i++) W[i] = 0.5f * zmin / W[i];   // 1/z, the nearest 0.5 (the engine's 2^31)
    float u0 = floorf(umin), v0 = floorf(vmin);
    int ext_s = (int)(umax - u0) + 1, ext_t = (int)(vmax - v0) + 1;
    if (ext_s > 3900 || ext_t > 3900) return 0;

    // the planes from the three vertices with the largest area on screen
    int a = 0, b = 1, c = 2;
    float best = 0;
    for (int j = 1; j + 1 < n; j++) {
        float d = (X[j] - X[0]) * (Y[j + 1] - Y[0]) - (X[j + 1] - X[0]) * (Y[j] - Y[0]);
        if (fabsf(d) > fabsf(best)) { best = d; b = j; c = j + 1; }
    }
    if (fabsf(best) < 2.0f) return 0;
    float id = 1.0f / best, X0 = X[a], Y0 = Y[a];
    float x1 = X[b] - X0, y1 = Y[b] - Y0, x2 = X[c] - X0, y2 = Y[c] - Y0;
#define PLANE(q0, q1, q2, o, dx, dy) do { \
        float d1_ = (q1) - (q0), d2_ = (q2) - (q0); \
        dx = (d1_ * y2 - d2_ * y1) * id; \
        dy = (x1 * d2_ - x2 * d1_) * id; \
        o = (q0) - X0 * dx - Y0 * dy; } while (0)
    float zo, zdx, zdy, so, sdx, sdy, to, tdx, tdy;
    PLANE(W[a], W[b], W[c], zo, zdx, zdy);
    PLANE((U[a] - u0) * W[a], (U[b] - u0) * W[b], (U[c] - u0) * W[c], so, sdx, sdy);
    PLANE((V[a] - v0) * W[a], (V[b] - v0) * W[b], (V[c] - v0) * W[c], to, tdx, tdy);
#undef PLANE
    int es = sp_exponent(so, sdx, sdy), et = sp_exponent(to, tdx, tdy);
    int32_t su = sp_fix(sdx, es), sv = sp_fix(sdy, es), s0 = sp_origin(so, sdx, sdy, su, sv, es);
    int32_t tu = sp_fix(tdx, et), tv = sp_fix(tdy, et), t0 = sp_origin(to, tdx, tdy, tu, tv, et);
    int32_t zu = sp_fix(zdx, 31), zv = sp_fix(zdy, 31), z0 = sp_origin(zo, zdx, zdy, zu, zv, 31);

    // the light, 256 - brightness, as a + b s + c t (least squares)
    float L[32];
    for (int i = 0; i < n; i++) L[i] = lit ? 256.0f - light_scale(vl[i].i) : 128.0f;
    float la = 0, lb = 0, lc = 0;
    {
        float S00 = (float)n, S0s = 0, S0t = 0, Sss = 0, Sst = 0, Stt = 0, R0 = 0, Rs = 0, Rt = 0;
        for (int i = 0; i < n; i++) {
            float s = U[i] - u0, t = V[i] - v0;
            S0s += s; S0t += t; Sss += s * s; Sst += s * t; Stt += t * t;
            R0 += L[i]; Rs += L[i] * s; Rt += L[i] * t;
        }
        float m00 = Sss * Stt - Sst * Sst, m01 = S0t * Sst - S0s * Stt, m02 = S0s * Sst - S0t * Sss;
        float dd = S00 * m00 + S0s * m01 + S0t * m02;
        if (fabsf(dd) > 1e-3f * (1.0f + Sss * Stt)) {
            float m11 = S00 * Stt - S0t * S0t, m12 = S0t * S0s - S00 * Sst, m22 = S00 * Sss - S0s * S0s;
            float idd = 1.0f / dd;
            la = (m00 * R0 + m01 * Rs + m02 * Rt) * idd;
            lb = (m01 * R0 + m11 * Rs + m12 * Rt) * idd;
            lc = (m02 * R0 + m12 * Rs + m22 * Rt) * idd;
        } else {
            la = R0 / (float)n;
        }
    }
    int ext = ext_s > ext_t ? ext_s : ext_t, k = 0;
    while ((ext >> k) > 6 && k < 7) k++;
    int lw = (ext_s >> k) + 2, lh = (ext_t >> k) + 2;
    if (lw > 31 || lh > 31) return 0;
    uint32_t bytes = (uint32_t)(lw * lh + 63) & ~63u;
    if (lm_pos + bytes > LM_RING) {
        m20_engine_wait();
        lm_pos = 0;
    }
    uint8_t *lm = lm_ring + lm_pos;
    lm_pos += bytes;
    for (int j = 0; j < lh; j++)
        for (int i = 0; i < lw; i++) {
            int q = (int)(la + lb * (float)(i << k) + lc * (float)(j << k) + 0.5f);
            lm[j * lw + i] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
        }
    cache_clean(lm, (uint32_t)(lw * lh));

    // the canvas's (0, 0) is the target's (the view canvas is copied to the screen there)
    uint32_t wl = (uint32_t)wlog, hl = (uint32_t)hlog, wm = (1u << wl) - 1u, hm = (1u << hl) - 1u;
    SPAN_HW[0] = LIT_B | 1u << 27 | (uint32_t)lh << 22 | (uint32_t)lw << 17 | SDRAM_OFF(lm) >> 6;
    SPAN_HW[0] = LIT_A | (uint32_t)k << 26 | hl << 22 | wl << 18 |
                 ((uint32_t)(int)v0 & hm) << 9 | ((uint32_t)(int)u0 & wm);
    SPAN_HW[1] = SDRAM_OFF(bits);
    SPAN_HW[2] = (uint32_t)es << 12 | (uint32_t)et << 18;
    SPAN_HW[3] = (uint32_t)s0;
    SPAN_HW[4] = (uint32_t)su;
    SPAN_HW[5] = (uint32_t)sv;
    SPAN_HW[6] = (uint32_t)t0;
    SPAN_HW[7] = (uint32_t)tu;
    SPAN_HW[8] = (uint32_t)tv;
    SPAN_HW[9] = (uint32_t)z0;
    SPAN_HW[10] = (uint32_t)zu;
    SPAN_HW[11] = (uint32_t)zv;
    SPAN_HW[12] = 0;
    SPAN_HW[13] = 0;
    SPAN_HW[14] = (uint32_t)(((ext_s - 1) << 16) | 0xFFFF);
    SPAN_HW[15] = (uint32_t)(((ext_t - 1) << 16) | 0xFFFF);
    hw_queued = 1;
    m20_engine_faces++;

    // The spans: a convex polygon, row by row, between its two chains of
    // edges from the top vertex (one each way round); each edge's x where
    // a row's top crosses it (Shock's rule: rows and columns from fix_cint,
    // the right and bottom edges left out), clipped to the 2D clip
    // rectangle as Shock's mappers are (the polygons reach under the view's
    // frame)
    int top = 0;
    for (int i = 1; i < n; i++) if (vl[i].y < vl[top].y) top = i;
    int y0 = cint(vl[top].y), ybot = y0;
    for (int i = 0; i < n; i++) { int yy = cint(vl[i].y); if (yy > ybot) ybot = yy; }
    int cl = grd_clip.left, ct = grd_clip.top, cr = grd_clip.right, cb = grd_clip.bot;
    if (cl < 0) cl = 0;
    if (ct < 0) ct = 0;
    if (cr > cv_w) cr = cv_w;
    if (cb > cv_h) cb = cv_h;
    if (y0 < ct) y0 = ct;
    if (ybot > cb) ybot = cb;
    uint8_t *rowp = cv->bm.bits;
    int row = cv->bm.row;
    struct { int i, dir, yend; fix x, dx; } ch[2] = {{top, 1, -1, 0, 0}, {top, -1, -1, 0, 0}};
    for (int y = y0; y < ybot; y++) {
        int ok = 1;
        for (int c = 0; c < 2; c++) {
            if (y < ch[c].yend) continue;
            // the chain's edge that row y's top crosses
            for (int guard = 0;; guard++) {
                int a = ch[c].i, b = (a + ch[c].dir + n) % n;
                if (guard >= n || vl[b].y < vl[a].y) { ok = 0; break; }   // (past the bottom)
                int yb = cint(vl[b].y);
                if (yb > y && vl[b].y > vl[a].y) {
                    float slope = (float)(vl[b].x - vl[a].x) / (float)(vl[b].y - vl[a].y);
                    ch[c].dx = (fix)(slope * 65536.0f);
                    ch[c].x = vl[a].x + (fix)(slope * (float)(((fix)y << 16) - vl[a].y));
                    ch[c].yend = yb;
                    break;
                }
                ch[c].i = b;
            }
        }
        if (!ok) break;
        fix xl = ch[0].x, xr = ch[1].x;
        if (xl > xr) { fix t = xl; xl = xr; xr = t; }
        ch[0].x += ch[0].dx;
        ch[1].x += ch[1].dx;
        int x0 = cint(xl), x1 = cint(xr);
        if (x0 < cl) x0 = cl;
        if (x1 > cr) x1 = cr;
        int cnt = x1 - x0;
        if (cnt <= 0) continue;
        SPAN_HW[0] = (uint32_t)cnt << 19 | (uint32_t)y << 10 | (uint32_t)x0;
        memset(rowp + y * row + x0, M20_HOLE, (size_t)cnt);
        m20_engine_spans++;
    }
    return 1;
}
#endif
