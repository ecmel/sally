// The machine as a whole: the XL memory map with 130XE banking, I/O
// dispatch, the PIA, the end of each scanline, resets and cartridges.

#include "machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Machine *machine_new(void) {
    static bool tables;
    if (!tables) {
        gtia_init();
        pokey_init();
        tables = true;
    }
    Machine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->pokey.cps = CPU_HZ / 48000.0;
    machine_set_os(m, NULL, 0);
    machine_cold_reset(m);
    return m;
}

void machine_free(Machine *m) {
    if (!m) return;
    free(m->cart);
    free(m);
}

bool machine_set_os(Machine *m, const uint8_t *os, size_t size) {
    if (os && size == sizeof m->os) {
        memcpy(m->os, os, size);
        return true;
    }
    // No ROM, or a bad one: the built-in OS.
    char err[128];
    if (!os_build(m->os, err, sizeof err)) fprintf(stderr, "built-in OS: %s\n", err);
    return os == NULL;
}

void machine_set_basic(Machine *m, const uint8_t *basic, size_t size) {
    m->has_basic = basic && size == sizeof m->basic;
    if (m->has_basic) memcpy(m->basic, basic, size);
    machine_update_memory_map(m);
}

const char *machine_load_cart(Machine *m, const uint8_t *data, size_t size) {
    int type = CART_STANDARD;
    // A .car file is a 16-byte header ("CART", type, checksum) and the image.
    // Types 1 and 2 are standard 8K and 16K, 26-32 MegaCarts of 16K to 1M,
    // 54-56 SIC! cartridges of 128K to 512K.
    if (size >= 16 && memcmp(data, "CART", 4) == 0) {
        uint32_t car = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
        data += 16;
        size -= 16;
        size_t want = 0;
        if (car == 1) want = 0x2000;
        if (car == 2) want = 0x4000;
        if (car >= 26 && car <= 32) {
            type = CART_MEGACART;
            want = (size_t)0x4000 << (car - 26);
        }
        if (car >= 54 && car <= 56) {
            type = CART_SIC;
            want = (size_t)0x20000 << (car - 54);
        }
        if (!want) return "Only standard 8K and 16K, MegaCart and SIC! cartridges are supported so far.";
        if (size != want) return "The cartridge image is not the size its type says.";
    } else if (size != 0x2000 && size != 0x4000) {
        return "A cartridge image must be 8K or 16K, or a .car file.";
    }
    uint8_t *cart = malloc(size);
    if (!cart) return "Out of memory.";
    memcpy(cart, data, size);
    free(m->cart);
    m->cart = cart;
    m->cart_size = size;
    m->cart_type = type;
    m->cart_bank = 0;
    machine_update_memory_map(m);
    return NULL;
}

void machine_eject_cart(Machine *m) {
    free(m->cart);
    m->cart = NULL;
    m->cart_size = 0;
    machine_update_memory_map(m);
}

// Maps 8K of ROM at page `first`, for the CPU and ANTIC.
static void map_rom(Machine *m, int first, uint8_t *rom) {
    for (int p = first; p < first + 0x20; p++) {
        m->rmap[p] = m->amap[p] = rom + (p - first) * 256;
        m->wmap[p] = m->sink;
    }
}

// The cartridge's two 8K windows, $8000-$9FFF and $A000-$BFFF, each NULL
// when it shows nothing there. A MegaCart maps a 16K bank at $8000, or
// none when bit 7 of its register is set. A SIC! maps the lower half of
// its bank at $8000 when bit 5 is set and the upper half at $A000 unless
// bit 6 is.
static void cart_windows(const Machine *m, uint8_t **lo, uint8_t **hi) {
    *lo = *hi = NULL;
    if (!m->cart) return;
    size_t mask = m->cart_size / 0x4000 - 1;
    uint8_t *bank = m->cart + (m->cart_bank & mask) * 0x4000;
    switch (m->cart_type) {
    case CART_STANDARD:
        if (m->cart_size == 0x4000) *lo = m->cart;
        *hi = m->cart + m->cart_size - 0x2000;
        break;
    case CART_MEGACART:
        if (!(m->cart_bank & 0x80)) {
            *lo = bank;
            *hi = bank + 0x2000;
        }
        break;
    case CART_SIC:
        if (m->cart_bank & 0x20) *lo = bank;
        if (!(m->cart_bank & 0x40)) *hi = bank + 0x2000;
        break;
    }
}

// PORTB on the XL: bit 0 enables the OS ROM, bit 1 low enables BASIC, bit
// 7 low maps the self test ROM at $5000. As on the 130XE, bits 2-3 pick a
// 16K bank of the extra RAM for $4000-$7FFF, which the CPU sees when bit 4
// is low and ANTIC when bit 5 is. Lines set as inputs read high.
void machine_update_memory_map(Machine *m) {
    uint8_t pb = (m->pia.orb & m->pia.ddrb) | ~m->pia.ddrb;
    for (int p = 0; p < 256; p++) m->rmap[p] = m->wmap[p] = m->amap[p] = m->ram + p * 256;
    uint8_t *bank = m->xram + ((pb >> 2) & 3) * 0x4000;
    for (int p = 0x40; p < 0x80; p++) {
        if (!(pb & 0x10)) m->rmap[p] = m->wmap[p] = bank + (p - 0x40) * 256;
        if (!(pb & 0x20)) m->amap[p] = bank + (p - 0x40) * 256;
    }
    if (pb & 0x01) {
        map_rom(m, 0xC0, m->os);
        map_rom(m, 0xE0, m->os + 0x2000);
        if (!(pb & 0x80)) {
            for (int p = 0x50; p < 0x58; p++) {
                m->rmap[p] = m->amap[p] = m->os + 0x1000 + (p - 0x50) * 256;
                m->wmap[p] = m->sink;
            }
        }
    }
    uint8_t *lo, *hi;
    cart_windows(m, &lo, &hi);
    if (lo) map_rom(m, 0x80, lo);
    if (hi) {
        map_rom(m, 0xA0, hi);
    } else if (m->has_basic && !(pb & 0x02)) {
        map_rom(m, 0xA0, m->basic);
    }
    m->rd5 = hi != NULL;
    for (int p = 0xD0; p < 0xD8; p++) m->rmap[p] = m->wmap[p] = m->amap[p] = NULL;
}

// The cartridge control area, $D500-$D5FF. A MegaCart takes a write
// anywhere in it as its bank register; a SIC! has its register at
// $D500-$D51F, readable too. Flash programming is not emulated.
static uint8_t cartctl_read(Machine *m, uint16_t addr) {
    if (m->cart && m->cart_type == CART_SIC && (addr & 0xE0) == 0) return m->cart_bank;
    return 0xFF;
}

static void cartctl_write(Machine *m, uint16_t addr, uint8_t v) {
    if (!m->cart) return;
    if (m->cart_type == CART_MEGACART || (m->cart_type == CART_SIC && (addr & 0xE0) == 0)) {
        m->cart_bank = v;
        machine_update_memory_map(m);
    }
}

static uint8_t pia_read(Machine *m, int reg) {
    Pia *p = &m->pia;
    switch (reg) {
    case 0:
        if (p->pactl & 4) {
            uint8_t in = (uint8_t)~(m->stick[0] | m->stick[1] << 4);
            return (p->ora & p->ddra) | (in & ~p->ddra);
        }
        return p->ddra;
    case 1:
        if (p->pbctl & 4) return (p->orb & p->ddrb) | ~p->ddrb;
        return p->ddrb;
    case 2: return p->pactl & 0x3F;
    default: return p->pbctl & 0x3F;
    }
}

static void pia_write(Machine *m, int reg, uint8_t v) {
    Pia *p = &m->pia;
    switch (reg) {
    case 0:
        if (p->pactl & 4)
            p->ora = v;
        else
            p->ddra = v;
        break;
    case 1:
        if (p->pbctl & 4)
            p->orb = v;
        else
            p->ddrb = v;
        machine_update_memory_map(m);
        break;
    case 2: p->pactl = v; break;
    default: p->pbctl = v; break;
    }
}

uint8_t io_read(Machine *m, uint16_t addr) {
    switch (addr >> 8) {
    case 0xD0: return gtia_read(m, addr & 0x1F);
    case 0xD2: return pokey_read(m, addr & 0x0F);
    case 0xD3: return pia_read(m, addr & 0x03);
    case 0xD4: return antic_read(m, addr & 0x0F);
    case 0xD5: return cartctl_read(m, addr);
    default: return 0xFF;
    }
}

void io_write(Machine *m, uint16_t addr, uint8_t v) {
    switch (addr >> 8) {
    case 0xD0: gtia_write(m, addr & 0x1F, v); break;
    case 0xD2: pokey_write(m, addr & 0x0F, v); break;
    case 0xD3: pia_write(m, addr & 0x03, v); break;
    case 0xD4: antic_write(m, addr & 0x0F, v); break;
    case 0xD5: cartctl_write(m, addr, v); break;
    default: break;
    }
}

void machine_end_line(Machine *m) {
    gtia_render(m, LINE_CLOCKS);
    m->gtia.cc = 0;
    m->x = 0;
    if (++m->line == LINES_PER_FRAME) m->line = 0;
    if (m->line >= FRAME_TOP && m->line < FRAME_BOTTOM) m->frame_hires[m->line - FRAME_TOP] = false;
    antic_start_line(m);
    if (m->line == FRAME_BOTTOM) m->frame_done = true;
}

void machine_run_frame(Machine *m) {
    m->frame_done = false;
    cpu_run(m);
    pokey_sync(m);
}

static void reset_chips(Machine *m) {
    memset(&m->antic, 0, sizeof m->antic);
    memset(&m->gtia, 0, sizeof m->gtia);
    memset(&m->pia, 0, sizeof m->pia);
    pokey_reset(m);
    m->nmi = false;
    m->irq = false;
    m->wsync = false;
    machine_update_memory_map(m);
}

// A bank-switched cartridge starts in bank 0; the RESET key leaves its
// register alone, as the XL's cartridge slot has no reset line.
void machine_cold_reset(Machine *m) {
    memset(m->ram, 0, sizeof m->ram);
    memset(m->xram, 0, sizeof m->xram);
    m->cart_bank = 0;
    m->x = 0;
    m->line = 0;
    reset_chips(m);
    memset(m->dma, 0, sizeof m->dma);
    memset(&m->cpu, 0, sizeof m->cpu);
    cpu_reset(m);
}

// The XL's RESET key resets the CPU and the PIA (so the OS ROM comes back);
// RAM is kept and the OS decides what survives.
void machine_warm_reset(Machine *m) {
    memset(&m->pia, 0, sizeof m->pia);
    machine_update_memory_map(m);
    m->antic.nmien = 0;
    m->nmi = false;
    m->wsync = false;
    cpu_reset(m);
}

void machine_key(Machine *m, uint8_t code, bool down) { pokey_key(m, code, down); }

void machine_break_key(Machine *m) { pokey_break(m); }
