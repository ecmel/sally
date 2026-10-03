// POKEY: four sound channels, their timer interrupts, the keyboard, the
// random number generator and enough of the serial port for the OS.
//
// Sound is event driven. Each channel's divider runs out at a known cycle;
// between those events the output level is constant, so it is integrated
// over each audio sample's span of cycles (a box filter) to resample the
// 1.79 MHz output. `pokey_sync` catches up to the CPU's clock, and runs
// before any write that changes the sound.

#include "machine.h"

#include <string.h>

static uint8_t poly4[15], poly5[31], poly9[511], poly17[131071];

static void make_poly(uint8_t *out, int bits, int tap) {
    int period = (1 << bits) - 1;
    uint32_t x = 1;
    for (int i = 0; i < period; i++) {
        out[i] = x & 1;
        x = (x >> 1) | (((x ^ (x >> tap)) & 1) << (bits - 1));
    }
}

void pokey_init(void) {
    make_poly(poly4, 4, 1);
    make_poly(poly5, 5, 2);
    make_poly(poly9, 9, 4);
    make_poly(poly17, 17, 5);
}

static bool running(const Pokey *p) { return p->skctl & 3; }

// CPU cycles between underflows of channel `ch`'s divider.
static uint64_t period(const Pokey *p, int ch) {
    uint8_t ac = p->audctl;
    int base = (ac & 0x01) ? 114 : 28;
    bool fast0 = ac & 0x40, fast2 = ac & 0x20;
    if (ch == 1 && (ac & 0x10)) {
        int f = p->audf[0] | p->audf[1] << 8;
        return fast0 ? f + 7 : (uint64_t)(f + 1) * base;
    }
    if (ch == 3 && (ac & 0x08)) {
        int f = p->audf[2] | p->audf[3] << 8;
        return fast2 ? f + 7 : (uint64_t)(f + 1) * base;
    }
    if ((ch == 0 && fast0) || (ch == 2 && fast2)) return p->audf[ch] + 4;
    return (uint64_t)(p->audf[ch] + 1) * base;
}

static void update_irq(Machine *m) {
    m->irq = (~m->pokey.irqst & m->pokey.irqen) != 0;
}

static void update_level(Machine *m) {
    Pokey *p = &m->pokey;
    int level = 0;
    for (int ch = 0; ch < 4; ch++) {
        uint8_t c = p->audc[ch];
        if (!(c & 0x0F)) continue;
        bool out = p->out[ch];
        // High-pass filters: channel 1 by channel 3, channel 2 by 4.
        if (ch == 0 && (p->audctl & 0x04)) out ^= p->hp[0];
        if (ch == 1 && (p->audctl & 0x02)) out ^= p->hp[1];
        if ((c & 0x10) || out) level += c & 0x0F;
    }
    if (!(m->gtia.consol_out & 0x08)) level += 8;
    p->level = level;
}

void pokey_refresh(Machine *m) { update_level(m); }

static void update_irq_next(Machine *m) {
    Pokey *p = &m->pokey;
    uint64_t next = ~(uint64_t)0;
    if (running(p)) {
        if ((p->irqen & 0x01) && p->next[0] < next) next = p->next[0];
        if ((p->irqen & 0x02) && p->next[1] < next) next = p->next[1];
        if ((p->irqen & 0x04) && p->next[3] < next) next = p->next[3];
    }
    p->irq_next = next;
}

static void emit(Pokey *p, double level) {
    // A DC blocker; the chip's output is unipolar.
    float x = (float)(level / 40.0);
    float y = x - p->dc_x + 0.997f * p->dc_y;
    p->dc_x = x;
    p->dc_y = y;
    if (p->naudio < AUDIO_MAX) p->audio[p->naudio++] = y;
}

// Integrates the current level from `p->t` to cycle `te`, emitting every
// sample whose span ends on the way.
static void integrate(Pokey *p, uint64_t te) {
    double t = (double)p->t, level = p->level;
    while (p->sample_next <= (double)te) {
        p->acc += level * (p->sample_next - t);
        t = p->sample_next;
        emit(p, p->acc / p->cps);
        p->acc = 0;
        p->sample_next += p->cps;
    }
    p->acc += level * ((double)te - t);
}

static void underflow(Machine *m, int ch, uint64_t t) {
    Pokey *p = &m->pokey;
    p->next[ch] = t + period(p, ch);
    uint8_t c = p->audc[ch];
    if ((c & 0x80) || poly5[t % 31]) {
        if (c & 0x20)
            p->out[ch] = !p->out[ch];
        else if (c & 0x40)
            p->out[ch] = poly4[t % 15];
        else
            p->out[ch] = (p->audctl & 0x80) ? poly9[t % 511] : poly17[t % 131071];
    }
    if (ch == 2) p->hp[0] = p->out[0];
    if (ch == 3) p->hp[1] = p->out[1];
    static const uint8_t timer_bit[4] = {0x01, 0x02, 0x00, 0x04};
    if (p->irqen & timer_bit[ch]) p->irqst &= ~timer_bit[ch];
}

void pokey_sync(Machine *m) {
    Pokey *p = &m->pokey;
    uint64_t now = m->clock;
    while (p->t < now) {
        uint64_t te = now;
        if (running(p))
            for (int ch = 0; ch < 4; ch++)
                if (p->next[ch] < te) te = p->next[ch];
        if (p->serout_at < te) te = p->serout_at;
        integrate(p, te);
        p->t = te;
        if (running(p))
            for (int ch = 0; ch < 4; ch++)
                if (p->next[ch] == te) underflow(m, ch, te);
        if (p->serout_at == te) {
            // The byte is out: ready for the next, and the shift register
            // is empty.
            p->serout_at = ~(uint64_t)0;
            p->irqst &= ~(p->irqen & 0x18);
        }
        update_level(m);
    }
    update_irq_next(m);
    update_irq(m);
}

void pokey_set_rate(Machine *m, double sample_rate) {
    m->pokey.cps = CPU_HZ / sample_rate;
}

void pokey_reset(Machine *m) {
    Pokey *p = &m->pokey;
    double cps = p->cps;
    memset(p, 0, sizeof *p);
    p->cps = cps;
    p->irqst = 0xFF;
    p->t = m->clock;
    p->sample_next = (double)m->clock + cps;
    p->serout_at = ~(uint64_t)0;
    p->irq_next = ~(uint64_t)0;
}

static void restart_counters(Machine *m) {
    Pokey *p = &m->pokey;
    for (int ch = 0; ch < 4; ch++) p->next[ch] = m->clock + period(p, ch);
}

uint8_t pokey_read(Machine *m, uint8_t reg) {
    Pokey *p = &m->pokey;
    switch (reg) {
    case 0x08: return 0x00;  // ALLPOT: every pot has finished
    case 0x09: return p->kbcode;
    case 0x0A: {
        // RANDOM: eight bits of the polynomial counter, which steps every cycle.
        if (!running(p)) return 0xFF;
        uint64_t t = m->clock;
        uint8_t v = 0;
        bool nine = p->audctl & 0x80;
        for (int i = 0; i < 8; i++) v = v << 1 | (nine ? poly9[(t + i) % 511] : poly17[(t + i) % 131071]);
        return v;
    }
    case 0x0D: return 0xFF;  // SERIN
    case 0x0E:
        pokey_sync(m);
        return p->irqst;
    case 0x0F: return 0xFF & ~(p->key_down ? 0x04 : 0) & ~(p->shift_down ? 0x08 : 0);
    default: return reg < 8 ? 228 : 0xFF;  // paddles at rest
    }
}

void pokey_write(Machine *m, uint8_t reg, uint8_t v) {
    Pokey *p = &m->pokey;
    pokey_sync(m);
    switch (reg) {
    case 0x00: case 0x02: case 0x04: case 0x06: p->audf[reg >> 1] = v; break;
    case 0x01: case 0x03: case 0x05: case 0x07: p->audc[reg >> 1] = v; break;
    case 0x08: p->audctl = v; break;
    case 0x09: restart_counters(m); break;  // STIMER
    case 0x0D:                              // SEROUT: ten bits at the channel 4 rate
        p->serout_at = m->clock + 20 * period(p, 3);
        break;
    case 0x0E:
        p->irqen = v;
        p->irqst |= ~v;
        break;
    case 0x0F: {
        bool was = running(p);
        p->skctl = v;
        if (!was && running(p)) restart_counters(m);
        break;
    }
    default: break;
    }
    update_level(m);
    update_irq_next(m);
    update_irq(m);
}

void pokey_key(Machine *m, uint8_t code, bool down) {
    Pokey *p = &m->pokey;
    if (down) {
        p->kbcode = code;
        p->key_down = true;
        p->shift_down = code & 0x40;
        if (p->irqen & 0x40) p->irqst &= ~0x40;
    } else {
        p->key_down = false;
        p->shift_down = false;
    }
    update_irq(m);
}

void pokey_break(Machine *m) {
    Pokey *p = &m->pokey;
    if (p->irqen & 0x80) p->irqst &= ~0x80;
    update_irq(m);
}
