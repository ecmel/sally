// The machine as a whole: the XL memory map, I/O dispatch, the PIA, the
// end of each scanline, resets and cartridges.

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
    // A .car file is a 16-byte header ("CART", type, checksum) and the image.
    if (size >= 16 && memcmp(data, "CART", 4) == 0) {
        uint32_t type = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
        data += 16;
        size -= 16;
        if (!((type == 1 && size == 0x2000) || (type == 2 && size == 0x4000)))
            return "Only standard 8K and 16K cartridges are supported so far.";
    }
    if (size != 0x2000 && size != 0x4000) return "A cartridge image must be 8K or 16K.";
    uint8_t *cart = malloc(size);
    if (!cart) return "Out of memory.";
    memcpy(cart, data, size);
    free(m->cart);
    m->cart = cart;
    m->cart_size = size;
    machine_update_memory_map(m);
    return NULL;
}

void machine_eject_cart(Machine *m) {
    free(m->cart);
    m->cart = NULL;
    m->cart_size = 0;
    machine_update_memory_map(m);
}

// PORTB on the XL: bit 0 enables the OS ROM, bit 1 low enables BASIC, bit
// 7 low maps the self test ROM at $5000. Lines set as inputs read high.
void machine_update_memory_map(Machine *m) {
    uint8_t pb = (m->pia.orb & m->pia.ddrb) | ~m->pia.ddrb;
    for (int p = 0; p < 256; p++) m->rmap[p] = m->wmap[p] = m->ram + p * 256;
    if (pb & 0x01) {
        for (int p = 0xC0; p < 0x100; p++) {
            m->rmap[p] = m->os + (p - 0xC0) * 256;
            m->wmap[p] = m->sink;
        }
        if (!(pb & 0x80)) {
            for (int p = 0x50; p < 0x58; p++) {
                m->rmap[p] = m->os + 0x1000 + (p - 0x50) * 256;
                m->wmap[p] = m->sink;
            }
        }
    }
    if (m->cart) {
        int first = 0xC0 - (int)(m->cart_size >> 8);
        for (int p = first; p < 0xC0; p++) {
            m->rmap[p] = m->cart + (p - first) * 256;
            m->wmap[p] = m->sink;
        }
    } else if (m->has_basic && !(pb & 0x02)) {
        for (int p = 0xA0; p < 0xC0; p++) {
            m->rmap[p] = m->basic + (p - 0xA0) * 256;
            m->wmap[p] = m->sink;
        }
    }
    for (int p = 0xD0; p < 0xD8; p++) m->rmap[p] = m->wmap[p] = NULL;
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
    default: return 0xFF;
    }
}

void io_write(Machine *m, uint16_t addr, uint8_t v) {
    switch (addr >> 8) {
    case 0xD0: gtia_write(m, addr & 0x1F, v); break;
    case 0xD2: pokey_write(m, addr & 0x0F, v); break;
    case 0xD3: pia_write(m, addr & 0x03, v); break;
    case 0xD4: antic_write(m, addr & 0x0F, v); break;
    default: break;
    }
}

void machine_end_line(Machine *m) {
    gtia_render(m, LINE_CLOCKS);
    m->gtia.cc = 0;
    m->x = 0;
    if (++m->line == LINES_PER_FRAME) m->line = 0;
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

void machine_cold_reset(Machine *m) {
    memset(m->ram, 0, sizeof m->ram);
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
