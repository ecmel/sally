// ANTIC: the display list, the cycles its DMA takes from the CPU, and the
// playfield it hands GTIA for each scanline.
//
// Each line is set up when the beam reaches it: the display list
// instruction is fetched, the line buffer filled on the first line of a
// mode line, and the playfield worked out from the line buffer and the
// character set. The DMA map then holds the CPU off the cycles ANTIC uses.

#include "machine.h"

#include <string.h>

static const uint8_t mode_lines[16] = {0, 0, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1};
// Bytes per line at the normal playfield width (160 color clocks).
static const uint8_t normal_bytes[16] = {0, 0, 40, 40, 40, 40, 20, 20, 10, 10, 20, 20, 20, 40, 40, 40};

static int pf_width(uint8_t dmactl) {
    static const int w[4] = {0, 128, 160, 192};
    return w[dmactl & 3];
}

// Horizontal scrolling fetches one width step more than is shown.
static int fetch_width(const Antic *a) {
    int w = pf_width(a->dmactl);
    if ((a->ir & 0x10) && w && w < 192) w += 32;
    return w;
}

static inline uint16_t scan_addr(uint16_t base, int i) {
    return (base & 0xF000) | ((base + i) & 0x0FFF);
}

// First cycle of playfield DMA for each fetch width.
static int dma_start(int width) {
    return width == 128 ? 26 : width == 160 ? 18 : 10;
}

static void playfield_dma(Machine *m, int mode, bool first) {
    Antic *a = &m->antic;
    int width = fetch_width(a);
    if (!width) return;
    int n = normal_bytes[mode] * width / 160;
    int step = 160 / normal_bytes[mode] / 2;
    int start = dma_start(width) + ((a->ir & 0x10) ? (a->hscrol >> 1) : 0);
    bool text = mode <= 7;
    for (int i = 0; i < n; i++) {
        int c = start + i * step;
        if (first && c < CYCLES_PER_LINE) m->dma[c] = 1;
        // Text modes fetch a glyph row for each character on every line.
        if (text && c + 3 < CYCLES_PER_LINE) m->dma[c + 3] = 1;
    }
}

static inline void put(Antic *a, int cc, int lo, int hi, uint8_t v) {
    if (cc >= lo && cc < hi) a->pf[cc] = v;
}

// Works out the playfield colors of the line from the line buffer.
static void build_playfield(Machine *m, int mode) {
    Antic *a = &m->antic;
    int width = pf_width(a->dmactl);
    if (!width) return;
    int lo = FRAME_LEFT + (192 - width) / 2, hi = lo + width;
    bool scrolled = a->ir & 0x10;
    int start = (scrolled && width < 192 ? lo - 16 : lo) + (scrolled ? a->hscrol : 0);
    const uint8_t *buf = a->linebuf;
    int n = a->nbytes;
    a->pf_start = start;
    a->pf_left = start > lo ? start : lo;
    a->pf_right = hi;

    switch (mode) {
    case 2:
    case 3:
    case 0xF: {
        // High resolution: a pixel per half color clock on PF2, lit pixels
        // taking PF1's luminance.
        a->hires = true;
        memset(a->hi, 0, sizeof a->hi);
        uint16_t base = (a->chbase & 0xFC) << 8;
        for (int i = 0; i < n; i++) {
            uint8_t g;
            if (mode == 0xF) {
                g = buf[i];
            } else {
                uint8_t ch = buf[i];
                int code = ch & 0x7F, row = a->row;
                if (mode == 2) {
                    row &= 7;
                    if (a->chactl & 4) row = 7 - row;
                } else if ((code & 0x60) == 0x60) {
                    // Mode 3 draws lowercase descenders in its last two rows.
                    row = row < 2 ? -1 : row < 8 ? row : row < 10 ? row - 8 : -1;
                } else if (row >= 8) {
                    row = -1;
                }
                g = row < 0 ? 0 : antic_peek(m, base + code * 8 + row);
                if (ch & 0x80) {
                    if (a->chactl & 1) g = 0;
                    if (a->chactl & 2) g ^= 0xFF;
                }
            }
            for (int k = 0; k < 4; k++) {
                int cc = start + i * 4 + k;
                if (cc >= lo && cc < hi) {
                    a->pf[cc] = 3;
                    a->hi[cc] = g >> (6 - 2 * k) & 3;
                }
            }
        }
        break;
    }
    case 4:
    case 5: {
        uint16_t base = (a->chbase & 0xFC) << 8;
        int row = (mode == 4 ? a->row : a->row >> 1) & 7;
        if (a->chactl & 4) row = 7 - row;
        for (int i = 0; i < n; i++) {
            uint8_t ch = buf[i];
            uint8_t g = antic_peek(m, base + (ch & 0x7F) * 8 + row);
            for (int k = 0; k < 4; k++) {
                int px = g >> (6 - 2 * k) & 3;
                if (px == 3 && (ch & 0x80)) px = 4;
                put(a, start + i * 4 + k, lo, hi, px);
            }
        }
        break;
    }
    case 6:
    case 7: {
        uint16_t base = (a->chbase & 0xFE) << 8;
        int row = (mode == 6 ? a->row : a->row >> 1) & 7;
        if (a->chactl & 4) row = 7 - row;
        for (int i = 0; i < n; i++) {
            uint8_t ch = buf[i];
            uint8_t g = antic_peek(m, base + (ch & 0x3F) * 8 + row);
            uint8_t color = (ch >> 6) + 1;
            for (int b = 0; b < 8; b++)
                if (g & (0x80 >> b)) put(a, start + i * 8 + b, lo, hi, color);
        }
        break;
    }
    default: {
        // Bitmaps: two bits a pixel in modes 8, A, D and E, one bit (PF0
        // on background) in 9, B and C.
        bool two = mode == 8 || mode == 0xA || mode == 0xD || mode == 0xE;
        int clocks = mode == 8 ? 4 : mode == 9 || mode == 0xA ? 2 : 1;
        int px_per_byte = two ? 4 : 8;
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            for (int k = 0; k < px_per_byte; k++) {
                int px = two ? b >> (6 - 2 * k) & 3 : b >> (7 - k) & 1;
                if (!px) continue;
                int cc = start + (i * px_per_byte + k) * clocks;
                for (int s = 0; s < clocks; s++) put(a, cc + s, lo, hi, px);
            }
        }
        break;
    }
    }
}

static void raise_nmi(Machine *m) {
    m->nmi = true;
    m->nmi_at = m->clock + 8;
}

void antic_start_line(Machine *m) {
    Antic *a = &m->antic;
    int line = m->line;
    memset(m->dma, 0, CYCLES_PER_LINE);
    memset(a->pf, 0, sizeof a->pf);
    a->hires = false;
    bool display = line >= FRAME_TOP && line < FRAME_BOTTOM;
    bool dli = false;
    if (line == FRAME_TOP) {
        a->left = 0;
        a->waiting = false;
        a->vscrolled = false;
    }

    if (display && (a->dmactl & 0x20) && !a->waiting) {
        bool first = false;
        if (a->left == 0) {
            uint8_t ir = antic_peek(m, a->dlist);
            a->dlist = (a->dlist & 0xFC00) | ((a->dlist + 1) & 0x03FF);
            m->dma[1] = 1;
            a->ir = ir;
            int mode = ir & 0x0F;
            if (mode == 0) {
                a->mode = 0;
                a->left = ((ir >> 4) & 7) + 1;
            } else if (mode == 1) {
                // JMP, or JVB (wait for vertical blank): one blank line.
                uint8_t lo = antic_peek(m, a->dlist);
                uint8_t hi = antic_peek(m, (a->dlist & 0xFC00) | ((a->dlist + 1) & 0x03FF));
                a->dlist = lo | hi << 8;
                m->dma[6] = m->dma[7] = 1;
                a->waiting = ir & 0x40;
                a->mode = 0;
                a->left = 1;
            } else {
                if (ir & 0x40) {
                    uint8_t lo = antic_peek(m, a->dlist);
                    uint8_t hi = antic_peek(m, (a->dlist & 0xFC00) | ((a->dlist + 1) & 0x03FF));
                    a->scan = lo | hi << 8;
                    a->dlist = (a->dlist & 0xFC00) | ((a->dlist + 2) & 0x03FF);
                    m->dma[6] = m->dma[7] = 1;
                }
                a->mode = mode;
                // Vertical scrolling starts the first scrolled mode line at
                // row VSCROL, and ends the one after the last at VSCROL.
                bool scrolled = ir & 0x20;
                int first_row = scrolled && !a->vscrolled ? a->vscrol : 0;
                int last_row = !scrolled && a->vscrolled ? a->vscrol : mode_lines[mode] - 1;
                a->vscrolled = scrolled;
                a->left = ((last_row - first_row) & 0x0F) + 1;
                a->row = first_row;
                first = true;
            }
        }
        dli = (a->ir & 0x80) && a->left == 1;
        if (a->mode) {
            if (first) {
                int n = normal_bytes[a->mode] * fetch_width(a) / 160;
                for (int i = 0; i < n; i++) a->linebuf[i] = antic_peek(m, scan_addr(a->scan, i));
                a->nbytes = n;
                a->scan = scan_addr(a->scan, n);
            }
            playfield_dma(m, a->mode, first);
            build_playfield(m, a->mode);
            a->row = (a->row + 1) & 0x0F;
        }
        a->left--;
    }
    if (display) gtia_pm_dma(m);

    // Nine memory refresh cycles, pushed later by any DMA in their way.
    for (int k = 0, c = 25; k < 9; k++, c += 4) {
        int r = c;
        while (r < CYCLES_PER_LINE && m->dma[r]) r++;
        if (r < CYCLES_PER_LINE) m->dma[r] = 1;
    }

    if (dli) {
        a->nmist = (a->nmist | 0x80) & ~0x40;
        if (a->nmien & 0x80) raise_nmi(m);
    }
    if (line == FRAME_BOTTOM) {
        a->nmist = (a->nmist | 0x40) & ~0x80;
        if (a->nmien & 0x40) raise_nmi(m);
    }
}

uint8_t antic_read(Machine *m, uint8_t reg) {
    switch (reg) {
    case 0x0B: return m->line >> 1;  // VCOUNT
    case 0x0C:
    case 0x0D: return 0;              // PENH, PENV
    case 0x0F: return m->antic.nmist | 0x1F;
    default: return 0xFF;
    }
}

void antic_write(Machine *m, uint8_t reg, uint8_t v) {
    Antic *a = &m->antic;
    switch (reg) {
    case 0x00: a->dmactl = v; break;
    case 0x01: a->chactl = v & 7; break;
    case 0x02: a->dlist = (a->dlist & 0xFF00) | v; break;
    case 0x03: a->dlist = (a->dlist & 0x00FF) | v << 8; break;
    case 0x04: a->hscrol = v & 0x0F; break;
    case 0x05: a->vscrol = v & 0x0F; break;
    case 0x07: a->pmbase = v; break;
    case 0x09: a->chbase = v; break;
    case 0x0A: m->wsync = true; break;
    case 0x0E: a->nmien = v; break;
    case 0x0F: a->nmist = 0; break;  // NMIRES
    default: break;
    }
}
