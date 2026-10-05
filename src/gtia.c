// GTIA: mixes ANTIC's playfield with players and missiles, resolves
// priorities, records collisions and writes color values to the frame.
//
// A line is drawn in segments. Before a register write takes effect, the
// line is drawn up to the color clock the beam has reached, so changes in
// the middle of a line land where they would on the screen.

#include "machine.h"

#include <string.h>

uint32_t gtia_palette[256];

// Priority: for each PRIOR setting (bits 0-3 and the multicolor bit 5),
// set of playfields (PF0-PF3) and set of players, which color registers
// show. Bits 0-3 are COLPM0-3, 4-7 COLPF0-3, 8 COLBK; when more than one
// is set, the colors are ORed, as on the chip.
static uint16_t prio[32][256];

// The NTSC palette at the eight luminances CTIA shows; the odd ones GTIA's
// mode 9 adds lie halfway between.
static const uint32_t base_palette[16][8] = {
    {0x000000, 0x404040, 0x6C6C6C, 0x909090, 0xB0B0B0, 0xC8C8C8, 0xDCDCDC, 0xECECEC},
    {0x444400, 0x646410, 0x848424, 0xA0A034, 0xB8B840, 0xD0D050, 0xE8E85C, 0xFCFC68},
    {0x702800, 0x844414, 0x985C28, 0xAC783C, 0xBC8C4C, 0xCCA05C, 0xDCB468, 0xECC878},
    {0x841800, 0x983418, 0xAC5030, 0xC06848, 0xD0805C, 0xE09470, 0xECA880, 0xFCBC94},
    {0x880000, 0x9C2020, 0xB03C3C, 0xC05858, 0xD07070, 0xE08888, 0xECA0A0, 0xFCB4B4},
    {0x78005C, 0x8C2074, 0xA03C88, 0xB0589C, 0xC070B0, 0xD084C0, 0xDC9CD0, 0xECB0E0},
    {0x480078, 0x602090, 0x783CA4, 0x8C58B8, 0xA070CC, 0xB484DC, 0xC49CEC, 0xD4B0FC},
    {0x140084, 0x302098, 0x4C3CAC, 0x6858C0, 0x7C70D0, 0x9488E0, 0xA8A0EC, 0xBCB4FC},
    {0x000088, 0x1C209C, 0x3840B0, 0x505CC0, 0x6874D0, 0x7C8CE0, 0x90A4EC, 0xA4B8FC},
    {0x00187C, 0x1C3890, 0x3854A8, 0x5070BC, 0x6888CC, 0x7C9CDC, 0x90B4EC, 0xA4C8FC},
    {0x002C5C, 0x1C4C78, 0x386890, 0x5084AC, 0x689CC0, 0x7CB4D4, 0x90CCE8, 0xA4E0FC},
    {0x003C2C, 0x1C5C48, 0x387C64, 0x509C80, 0x68B494, 0x7CD0AC, 0x90E4C0, 0xA4FCD4},
    {0x003C00, 0x205C20, 0x407C40, 0x5C9C5C, 0x74B474, 0x8CD08C, 0xA4E4A4, 0xB8FCB8},
    {0x143800, 0x345C1C, 0x507C38, 0x6C9850, 0x84B468, 0x9CCC7C, 0xB4E490, 0xC8FCA4},
    {0x2C3000, 0x4C501C, 0x687034, 0x848C4C, 0x9CA864, 0xB4C078, 0xCCD488, 0xE0EC9C},
    {0x442800, 0x644818, 0x846830, 0xA08444, 0xB89C58, 0xD0B46C, 0xE8CC7C, 0xFCE08C},
};

static uint32_t blend(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) {
        int v = ((int)(a >> s & 0xFF) + (int)(b >> s & 0xFF)) / 2;
        r |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << s;
    }
    return r;
}

void gtia_init(void) {
    for (int h = 0; h < 16; h++) {
        for (int l = 0; l < 8; l++) {
            uint32_t c = base_palette[h][l];
            uint32_t next = l < 7 ? base_palette[h][l + 1] : c;
            gtia_palette[h << 4 | l << 1] = c;
            gtia_palette[h << 4 | l << 1 | 1] = l < 7 ? blend(c, next) : c;
        }
    }
    // The chip's priority equations.
    for (int pri = 0; pri < 32; pri++) {
        bool p0r = pri & 1, p1r = pri & 2, p2r = pri & 4, p3r = pri & 8, multi = pri & 16;
        bool pri01 = p0r || p1r, pri12 = p1r || p2r, pri23 = p2r || p3r, pri03 = p0r || p3r;
        for (int i = 0; i < 256; i++) {
            bool pf0 = i & 0x10, pf1 = i & 0x20, pf2 = i & 0x40, pf3 = i & 0x80;
            bool pl0 = i & 1, pl1 = i & 2, pl2 = i & 4, pl3 = i & 8;
            bool p01 = pl0 || pl1, p23 = pl2 || pl3, pf01 = pf0 || pf1, pf23 = pf2 || pf3;
            bool sp0 = pl0 && !(pf01 && pri23) && !(p2r && pf23);
            bool sp1 = pl1 && !(pf01 && pri23) && !(p2r && pf23) && (!pl0 || multi);
            bool sp2 = pl2 && !p01 && !(pf23 && pri12) && !(pf01 && !p0r);
            bool sp3 = pl3 && !p01 && !(pf23 && pri12) && !(pf01 && !p0r) && (!pl2 || multi);
            bool sf3 = pf3 && !(p23 && pri03) && !(p01 && !p2r);
            bool sf0 = pf0 && !(p23 && p0r) && !(p01 && pri01) && !sf3;
            bool sf1 = pf1 && !(p23 && p0r) && !(p01 && pri01) && !sf3;
            bool sf2 = pf2 && !(p23 && pri03) && !(p01 && !p2r) && !sf3;
            bool sb = !p01 && !p23 && !pf01 && !pf23;
            prio[pri][i] = sp0 | sp1 << 1 | sp2 << 2 | sp3 << 3 | sf0 << 4 | sf1 << 5 | sf2 << 6 | sf3 << 7 | sb << 8;
        }
    }
}

static const uint8_t pm_width[4] = {1, 2, 1, 4};

// Marks the color clocks in [start, end) each player (bits 0-3) and missile
// (bits 4-7) covers. Returns whether any does.
static bool build_pm(const Gtia *g, uint8_t *pm, int start, int end) {
    bool any = false;
    memset(pm + start, 0, end - start);
    for (int p = 0; p < 4; p++) {
        uint8_t graf = g->grafp[p];
        if (!graf) continue;
        int w = pm_width[g->sizep[p] & 3], x = g->hpos[p];
        if (x >= end || x + 8 * w <= start) continue;
        for (int b = 0; b < 8; b++) {
            if (!(graf & (0x80 >> b))) continue;
            for (int s = 0; s < w; s++) {
                int cc = x + b * w + s;
                if (cc >= start && cc < end) {
                    pm[cc] |= 1 << p;
                    any = true;
                }
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        int bits = g->grafm >> (2 * i) & 3;
        if (!bits) continue;
        int w = pm_width[g->sizem >> (2 * i) & 3], x = g->hpos[4 + i];
        for (int b = 0; b < 2; b++) {
            if (!(bits & (2 >> b))) continue;
            for (int s = 0; s < w; s++) {
                int cc = x + b * w + s;
                if (cc >= start && cc < end) {
                    pm[cc] |= 0x10 << i;
                    any = true;
                }
            }
        }
    }
    return any;
}

static inline void collide(Gtia *g, uint8_t objs, uint8_t pf) {
    uint8_t players = objs & 0x0F;
    for (int p = 0; p < 4; p++) {
        if (players & (1 << p)) {
            g->coll[4 + p] |= pf;
            g->coll[12 + p] |= players & ~(1 << p);
        }
        if (objs & (0x10 << p)) {
            g->coll[p] |= pf;
            g->coll[8 + p] |= players;
        }
    }
}

// The color where players and missiles `objs` meet playfields `pf` (a
// PF0-PF3 bit set).
static inline uint8_t resolve(const Gtia *g, const uint8_t *col, int pri, uint8_t objs, uint8_t pf) {
    uint8_t players = objs & 0x0F, missiles = objs >> 4;
    // The fifth player: missiles take PF3's color and priority.
    if (g->prior & 0x10) {
        if (missiles) pf |= 8;
    } else {
        players |= missiles;
    }
    uint16_t mask = prio[pri][pf << 4 | players];
    uint8_t c = 0;
    while (mask) {
        c |= col[__builtin_ctz(mask)];
        mask &= mask - 1;
    }
    return c;
}

static void draw(Machine *m, int start, int end) {
    Gtia *g = &m->gtia;
    const Antic *a = &m->antic;
    uint8_t *out = m->frame + (m->line - FRAME_TOP) * FRAME_WIDTH + (start - FRAME_LEFT) * 2;
    uint8_t col[9];
    for (int i = 0; i < 9; i++) col[i] = g->col[i] & 0xFE;
    const uint8_t pfcol[5] = {col[8], col[4], col[5], col[6], col[7]};
    uint8_t pm[LINE_CLOCKS];
    bool any = build_pm(g, pm, start, end);
    int pri = (g->prior & 0x0F) | (g->prior & 0x20) >> 1;
    int gmode = g->prior >> 6;

    if (gmode && a->hires) {
        // GTIA modes 9-11: each four high resolution pixels (two color
        // clocks) form one wide pixel from a 4-bit value.
        static const uint8_t mode10[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 4, 5, 6, 7};
        for (int cc = start; cc < end; cc++, out += 2) {
            int nib = 0;
            if (cc >= a->pf_left && cc < a->pf_right) {
                int c0 = a->pf_start + ((cc - a->pf_start) & ~1);
                nib = a->hi[c0] << 2 | a->hi[c0 + 1];
            }
            uint8_t c;
            if (gmode == 1)
                c = (g->col[8] & 0xF0) | nib;
            else if (gmode == 2)
                c = g->col[mode10[nib]] & 0xFE;
            else
                c = nib << 4 | (g->col[8] & 0x0E);
            if (any && pm[cc]) {
                collide(g, pm[cc], 0);
                c = resolve(g, col, pri, pm[cc], 0);
            }
            out[0] = out[1] = c;
        }
        return;
    }

    if (!a->hires) {
        if (!any) {
            for (int cc = start; cc < end; cc++, out += 2) out[0] = out[1] = pfcol[a->pf[cc]];
            return;
        }
        for (int cc = start; cc < end; cc++, out += 2) {
            uint8_t objs = pm[cc], p = a->pf[cc];
            if (!objs) {
                out[0] = out[1] = pfcol[p];
                continue;
            }
            uint8_t pf = p ? 1 << (p - 1) : 0;
            collide(g, objs, pf);
            out[0] = out[1] = resolve(g, col, pri, objs, pf);
        }
        return;
    }

    // High resolution: the playfield is PF2 for priority; lit pixels take
    // PF1's luminance whatever is on top, and collide as PF2.
    uint8_t lum = col[5] & 0x0F;
    m->frame_hires[m->line - FRAME_TOP] = true;
    for (int cc = start; cc < end; cc++, out += 2) {
        uint8_t objs = any ? pm[cc] : 0, p = a->pf[cc], h = p ? a->hi[cc] : 0;
        uint8_t c;
        if (objs) {
            collide(g, objs, h ? 4 : 0);
            c = resolve(g, col, pri, objs, p ? 4 : 0);
        } else {
            c = pfcol[p];
        }
        out[0] = h & 2 ? (c & 0xF0) | lum : c;
        out[1] = h & 1 ? (c & 0xF0) | lum : c;
    }
}

void gtia_render(Machine *m, int end) {
    Gtia *g = &m->gtia;
    int start = g->cc;
    if (end > LINE_CLOCKS) end = LINE_CLOCKS;
    if (end <= start) return;
    g->cc = end;
    if (m->line < FRAME_TOP || m->line >= FRAME_BOTTOM) return;
    if (start < FRAME_LEFT) start = FRAME_LEFT;
    if (end > FRAME_LEFT + FRAME_CLOCKS) end = FRAME_LEFT + FRAME_CLOCKS;
    if (end > start) draw(m, start, end);
}

// Where the beam is when the CPU writes on the current cycle.
static inline int beam(const Machine *m) { return 2 * m->x + 1; }

// Player and missile DMA at the start of a display line, into the GRAF
// registers. In two-line resolution VDELAY holds an object's data back to
// odd lines.
void gtia_pm_dma(Machine *m) {
    Antic *a = &m->antic;
    Gtia *g = &m->gtia;
    uint8_t dmactl = a->dmactl;
    if (!(dmactl & 0x0C)) return;
    int line = m->line;
    bool single = dmactl & 0x10;
    uint16_t base = single ? (a->pmbase & 0xF8) << 8 : (a->pmbase & 0xFC) << 8;
    m->dma[0] = 1;
    if (g->gractl & 1) {
        uint8_t v = antic_peek(m, single ? base + 0x300 + line : base + 0x180 + (line >> 1));
        for (int i = 0; i < 4; i++) {
            if (!single && (g->vdelay & (1 << i)) && !(line & 1)) continue;
            uint8_t mask = 3 << (2 * i);
            g->grafm = (g->grafm & ~mask) | (v & mask);
        }
    }
    if (dmactl & 0x08) {
        for (int p = 0; p < 4; p++) m->dma[2 + p] = 1;
        if (g->gractl & 2) {
            for (int p = 0; p < 4; p++) {
                if (!single && (g->vdelay & (0x10 << p)) && !(line & 1)) continue;
                g->grafp[p] = antic_peek(m, single ? base + 0x400 + p * 0x100 + line : base + 0x200 + p * 0x80 + (line >> 1));
            }
        }
    }
}

static bool trigger(Machine *m, int i) {
    Gtia *g = &m->gtia;
    if (m->trig[i]) g->trig_latch |= 1 << i;
    return (g->gractl & 4) ? g->trig_latch & (1 << i) : m->trig[i];
}

uint8_t gtia_read(Machine *m, uint8_t reg) {
    Gtia *g = &m->gtia;
    if (reg < 0x10) {
        gtia_render(m, beam(m));
        return g->coll[reg] & 0x0F;
    }
    switch (reg) {
    case 0x10:
    case 0x11: return !trigger(m, reg - 0x10);
    case 0x12: return 1;
    case 0x13: return m->rd5;  // the XL reads cartridge presence here
    case 0x14: return 0x0F;  // PAL: an NTSC machine
    case 0x1F: return ~m->consol_keys & 0x07;  // the upper bits read as 0
    default: return 0x0F;
    }
}

void gtia_write(Machine *m, uint8_t reg, uint8_t v) {
    Gtia *g = &m->gtia;
    if (reg <= 0x1C || reg == 0x1E) gtia_render(m, beam(m));
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07: g->hpos[reg] = v; break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: g->sizep[reg - 0x08] = v; break;
    case 0x0C: g->sizem = v; break;
    case 0x0D: case 0x0E: case 0x0F: case 0x10: g->grafp[reg - 0x0D] = v; break;
    case 0x11: g->grafm = v; break;
    case 0x1B: g->prior = v; break;
    case 0x1C: g->vdelay = v; break;
    case 0x1D:
        g->gractl = v;
        if (!(v & 4)) g->trig_latch = 0;
        break;
    case 0x1E: memset(g->coll, 0, sizeof g->coll); break;
    case 0x1F:
        // Bit 3 drives the console speaker.
        pokey_sync(m);
        g->consol_out = v;
        pokey_refresh(m);
        break;
    default: g->col[reg - 0x12] = v; break;
    }
}
