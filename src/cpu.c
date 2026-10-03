// The 6502C ("Sally"): an NMOS 6502 with a HALT input that lets ANTIC
// take any cycle. Every bus access is one cycle, including the dummy reads
// and writes the 6502 makes, so registers are written on the cycle they
// would be on the machine. Undocumented opcodes are included.

#include "machine.h"

// Moves the clock one cycle; the line ends after cycle 113.
static inline void advance(Machine *m) {
    m->clock++;
    if (++m->x == CYCLES_PER_LINE) machine_end_line(m);
}

// Waits for a cycle ANTIC leaves to the CPU.
static inline void wait_free(Machine *m) {
    if (m->wsync) machine_wait_wsync(m);
    while (m->dma[m->x]) advance(m);
}

// WSYNC holds the CPU (through RDY) until cycle 105, of the next line if
// the write came after it.
void machine_wait_wsync(Machine *m) {
    m->wsync = false;
    if (m->x > 105)
        while (m->x != 0) advance(m);
    while (m->x < 105) advance(m);
}

static inline uint8_t rd(Machine *m, uint16_t a) {
    wait_free(m);
    m->cpu_cycles++;
    m->last_access = m->clock;
    const uint8_t *p = m->rmap[a >> 8];
    uint8_t v = p ? p[a & 0xFF] : io_read(m, a);
    advance(m);
    return v;
}

static inline void wr(Machine *m, uint16_t a, uint8_t v) {
    wait_free(m);
    m->cpu_cycles++;
    m->last_access = m->clock;
    uint8_t *p = m->wmap[a >> 8];
    if (p)
        p[a & 0xFF] = v;
    else
        io_write(m, a, v);
    advance(m);
}

// A cycle whose access has no effect (a dummy read).
static inline void idle(Machine *m) {
    wait_free(m);
    m->cpu_cycles++;
    m->last_access = m->clock;
    advance(m);
}

static inline uint8_t fetch(Machine *m) { return rd(m, m->cpu.pc++); }

static inline uint16_t fetch16(Machine *m) {
    uint8_t lo = fetch(m);
    uint8_t hi = fetch(m);
    return lo | hi << 8;
}

static inline void push(Machine *m, uint8_t v) { wr(m, 0x100 | m->cpu.s--, v); }
static inline uint8_t pull(Machine *m) { return rd(m, 0x100 | ++m->cpu.s); }

static inline uint8_t get_p(const Cpu *c, bool brk) {
    return c->n << 7 | c->v << 6 | 0x20 | brk << 4 | c->d << 3 | c->i << 2 | c->z << 1 | c->c;
}

static inline void set_p(Cpu *c, uint8_t p) {
    c->n = p >> 7 & 1;
    c->v = p >> 6 & 1;
    c->d = p >> 3 & 1;
    c->i = p >> 2 & 1;
    c->z = p >> 1 & 1;
    c->c = p & 1;
}

static inline uint8_t nz(Cpu *c, uint8_t v) {
    c->n = v >> 7;
    c->z = v == 0;
    return v;
}

// ADC and SBC with the NMOS 6502's decimal mode: N and V come from the
// intermediate result, Z from the binary one.
static void adc(Cpu *c, uint8_t b) {
    int a = c->a, cin = c->c;
    if (!c->d) {
        int s = a + b + cin;
        c->v = (~(a ^ b) & (a ^ s) & 0x80) != 0;
        c->c = s > 0xFF;
        c->a = nz(c, (uint8_t)s);
        return;
    }
    int lo = (a & 0x0F) + (b & 0x0F) + cin;
    if (lo >= 0x0A) lo = ((lo + 0x06) & 0x0F) + 0x10;
    int s = (a & 0xF0) + (b & 0xF0) + lo;
    int signed_s = (int8_t)(a & 0xF0) + (int8_t)(b & 0xF0) + lo;
    c->z = ((a + b + cin) & 0xFF) == 0;
    c->n = (s & 0x80) != 0;
    c->v = signed_s < -128 || signed_s > 127;
    if (s >= 0xA0) s += 0x60;
    c->c = s >= 0x100;
    c->a = (uint8_t)s;
}

static void sbc(Cpu *c, uint8_t b) {
    int a = c->a, borrow = !c->c;
    int bin = a - b - borrow;
    c->v = ((a ^ b) & (a ^ bin) & 0x80) != 0;
    c->c = bin >= 0;
    nz(c, (uint8_t)bin);
    if (!c->d) {
        c->a = (uint8_t)bin;
        return;
    }
    int lo = (a & 0x0F) - (b & 0x0F) - borrow;
    if (lo < 0) lo = ((lo - 0x06) & 0x0F) - 0x10;
    int s = (a & 0xF0) - (b & 0xF0) + lo;
    if (s < 0) s -= 0x60;
    c->a = (uint8_t)s;
}

static inline void cmp(Cpu *c, uint8_t r, uint8_t v) {
    c->c = r >= v;
    nz(c, (uint8_t)(r - v));
}

static inline uint8_t asl(Cpu *c, uint8_t v) { c->c = v >> 7; return nz(c, v << 1); }
static inline uint8_t lsr(Cpu *c, uint8_t v) { c->c = v & 1; return nz(c, v >> 1); }
static inline uint8_t rol(Cpu *c, uint8_t v) { uint8_t r = v << 1 | c->c; c->c = v >> 7; return nz(c, r); }
static inline uint8_t ror(Cpu *c, uint8_t v) { uint8_t r = v >> 1 | c->c << 7; c->c = v & 1; return nz(c, r); }

static void arr(Cpu *c, uint8_t v) {
    uint8_t t = c->a & v;
    uint8_t r = t >> 1 | c->c << 7;
    if (!c->d) {
        c->a = nz(c, r);
        c->c = r >> 6 & 1;
        c->v = ((r >> 6) ^ (r >> 5)) & 1;
        return;
    }
    // Decimal ARR fixes up each nibble after the rotate.
    c->n = c->c;
    c->z = r == 0;
    c->v = ((t ^ r) & 0x40) != 0;
    if ((t & 0x0F) + (t & 0x01) > 5) r = (r & 0xF0) | ((r + 6) & 0x0F);
    c->c = (t >> 4) + ((t >> 4) & 1) > 5;
    if (c->c) r += 0x60;
    c->a = r;
}

// Hardware interrupts and BRK: push PC and P, then jump through `vector`.
static void interrupt(Machine *m, uint16_t vector, bool brk) {
    Cpu *c = &m->cpu;
    if (brk) {
        fetch(m);  // the padding byte after BRK
    } else {
        idle(m);
        idle(m);
    }
    push(m, c->pc >> 8);
    push(m, c->pc & 0xFF);
    push(m, get_p(c, brk));
    c->i = true;
    uint8_t lo = rd(m, vector);
    uint8_t hi = rd(m, vector + 1);
    c->pc = lo | hi << 8;
}

void cpu_reset(Machine *m) {
    Cpu *c = &m->cpu;
    c->s -= 3;
    c->i = true;
    c->d = false;
    c->jammed = false;
    c->pc = m->rmap[0xFF][0xFC] | m->rmap[0xFF][0xFD] << 8;
}

// Effective addresses, with the cycles each mode spends before its final
// access. Indexed modes read the unfixed address first when the index
// crosses a page; writes and read-modify-writes always do.
#define ZP() ea = fetch(m)
#define ZPX() do { ea = fetch(m); idle(m); ea = (uint8_t)(ea + c->x); } while (0)
#define ZPY() do { ea = fetch(m); idle(m); ea = (uint8_t)(ea + c->y); } while (0)
#define ABS() ea = fetch16(m)
#define ABI_R(r) do { uint16_t b_ = fetch16(m); ea = b_ + c->r; if ((b_ ^ ea) & 0xFF00) idle(m); } while (0)
#define ABI_W(r) do { uint16_t b_ = fetch16(m); ea = b_ + c->r; idle(m); } while (0)
#define IZX() do { uint8_t z_ = fetch(m); idle(m); z_ += c->x; uint8_t lo_ = rd(m, z_); uint8_t hi_ = rd(m, (uint8_t)(z_ + 1)); ea = lo_ | hi_ << 8; } while (0)
#define IZY_BASE() uint8_t z_ = fetch(m); uint8_t lo_ = rd(m, z_); uint8_t hi_ = rd(m, (uint8_t)(z_ + 1)); uint16_t b_ = lo_ | hi_ << 8; ea = b_ + c->y
#define IZY_R() do { IZY_BASE(); if ((b_ ^ ea) & 0xFF00) idle(m); } while (0)
#define IZY_W() do { IZY_BASE(); idle(m); } while (0)

// The eight addressing modes of a read instruction in column 1 (ORA, AND,
// EOR, ADC, LDA, CMP, SBC).
#define READ_GROUP(base, OP) \
    case base + 0x01: IZX(); v = rd(m, ea); OP; break; \
    case base + 0x05: ZP(); v = rd(m, ea); OP; break; \
    case base + 0x09: v = fetch(m); OP; break; \
    case base + 0x0D: ABS(); v = rd(m, ea); OP; break; \
    case base + 0x11: IZY_R(); v = rd(m, ea); OP; break; \
    case base + 0x15: ZPX(); v = rd(m, ea); OP; break; \
    case base + 0x19: ABI_R(y); v = rd(m, ea); OP; break; \
    case base + 0x1D: ABI_R(x); v = rd(m, ea); OP; break;

// Read-modify-write: read, write the old value back, write the new one.
#define RMW(OP) do { v = rd(m, ea); wr(m, ea, v); v = OP; wr(m, ea, v); } while (0)

#define RMW_GROUP(base, OP) \
    case base + 0x06: ZP(); RMW(OP); break; \
    case base + 0x0E: ABS(); RMW(OP); break; \
    case base + 0x16: ZPX(); RMW(OP); break; \
    case base + 0x1E: ABI_W(x); RMW(OP); break;

// The undocumented read-modify-write combinations (SLO, RLA, SRE, RRA,
// DCP, ISC) in column 3.
#define COMBO_GROUP(base, OP, THEN) \
    case base + 0x03: IZX(); RMW(OP); THEN; break; \
    case base + 0x07: ZP(); RMW(OP); THEN; break; \
    case base + 0x0F: ABS(); RMW(OP); THEN; break; \
    case base + 0x13: IZY_W(); RMW(OP); THEN; break; \
    case base + 0x17: ZPX(); RMW(OP); THEN; break; \
    case base + 0x1B: ABI_W(y); RMW(OP); THEN; break; \
    case base + 0x1F: ABI_W(x); RMW(OP); THEN; break;

#define BRANCH(cond) do { \
    int8_t off_ = (int8_t)fetch(m); \
    if (cond) { \
        idle(m); \
        uint16_t t_ = c->pc + off_; \
        if ((t_ ^ c->pc) & 0xFF00) idle(m); \
        c->pc = t_; \
    } \
} while (0)

// SHA, SHX, SHY, TAS: store `val & (high byte of the base + 1)`. When the
// index crosses a page, the stored value also replaces the high byte of
// the address.
#define STORE_HIGH(val, idx) do { \
    uint16_t b_ = fetch16(m); \
    ea = b_ + c->idx; \
    idle(m); \
    uint8_t s_ = (val) & ((b_ >> 8) + 1); \
    if ((b_ ^ ea) & 0xFF00) ea = (ea & 0xFF) | s_ << 8; \
    wr(m, ea, s_); \
} while (0)

static inline void step(Machine *m) {
    Cpu *c = &m->cpu;
    uint16_t ea;
    uint8_t v;

    if (m->clock >= m->pokey.irq_next || m->clock >= m->pokey.serout_at) pokey_sync(m);
    // An interrupt is taken if it was raised before the last cycle of the
    // instruction that just ended.
    if (m->nmi && m->nmi_at < m->last_access) {
        m->nmi = false;
        interrupt(m, 0xFFFA, false);
        return;
    }
    if (m->irq && !c->i) {
        interrupt(m, 0xFFFE, false);
        return;
    }
    if (c->jammed) {
        idle(m);
        return;
    }

    uint8_t op = fetch(m);
    switch (op) {
    READ_GROUP(0x00, c->a = nz(c, c->a | v))
    READ_GROUP(0x20, c->a = nz(c, c->a & v))
    READ_GROUP(0x40, c->a = nz(c, c->a ^ v))
    READ_GROUP(0x60, adc(c, v))
    READ_GROUP(0xA0, c->a = nz(c, v))
    READ_GROUP(0xC0, cmp(c, c->a, v))
    READ_GROUP(0xE0, sbc(c, v))

    // STA
    case 0x81: IZX(); wr(m, ea, c->a); break;
    case 0x85: ZP(); wr(m, ea, c->a); break;
    case 0x8D: ABS(); wr(m, ea, c->a); break;
    case 0x91: IZY_W(); wr(m, ea, c->a); break;
    case 0x95: ZPX(); wr(m, ea, c->a); break;
    case 0x99: ABI_W(y); wr(m, ea, c->a); break;
    case 0x9D: ABI_W(x); wr(m, ea, c->a); break;
    // STX, STY
    case 0x86: ZP(); wr(m, ea, c->x); break;
    case 0x8E: ABS(); wr(m, ea, c->x); break;
    case 0x96: ZPY(); wr(m, ea, c->x); break;
    case 0x84: ZP(); wr(m, ea, c->y); break;
    case 0x8C: ABS(); wr(m, ea, c->y); break;
    case 0x94: ZPX(); wr(m, ea, c->y); break;
    // LDX, LDY
    case 0xA2: c->x = nz(c, fetch(m)); break;
    case 0xA6: ZP(); c->x = nz(c, rd(m, ea)); break;
    case 0xAE: ABS(); c->x = nz(c, rd(m, ea)); break;
    case 0xB6: ZPY(); c->x = nz(c, rd(m, ea)); break;
    case 0xBE: ABI_R(y); c->x = nz(c, rd(m, ea)); break;
    case 0xA0: c->y = nz(c, fetch(m)); break;
    case 0xA4: ZP(); c->y = nz(c, rd(m, ea)); break;
    case 0xAC: ABS(); c->y = nz(c, rd(m, ea)); break;
    case 0xB4: ZPX(); c->y = nz(c, rd(m, ea)); break;
    case 0xBC: ABI_R(x); c->y = nz(c, rd(m, ea)); break;
    // CPX, CPY, BIT
    case 0xE0: cmp(c, c->x, fetch(m)); break;
    case 0xE4: ZP(); cmp(c, c->x, rd(m, ea)); break;
    case 0xEC: ABS(); cmp(c, c->x, rd(m, ea)); break;
    case 0xC0: cmp(c, c->y, fetch(m)); break;
    case 0xC4: ZP(); cmp(c, c->y, rd(m, ea)); break;
    case 0xCC: ABS(); cmp(c, c->y, rd(m, ea)); break;
    case 0x24: ZP(); v = rd(m, ea); c->n = v >> 7; c->v = v >> 6 & 1; c->z = (c->a & v) == 0; break;
    case 0x2C: ABS(); v = rd(m, ea); c->n = v >> 7; c->v = v >> 6 & 1; c->z = (c->a & v) == 0; break;

    RMW_GROUP(0x00, asl(c, v))
    RMW_GROUP(0x20, rol(c, v))
    RMW_GROUP(0x40, lsr(c, v))
    RMW_GROUP(0x60, ror(c, v))
    RMW_GROUP(0xC0, nz(c, v - 1))
    RMW_GROUP(0xE0, nz(c, v + 1))
    case 0x0A: idle(m); c->a = asl(c, c->a); break;
    case 0x2A: idle(m); c->a = rol(c, c->a); break;
    case 0x4A: idle(m); c->a = lsr(c, c->a); break;
    case 0x6A: idle(m); c->a = ror(c, c->a); break;

    case 0x10: BRANCH(!c->n); break;
    case 0x30: BRANCH(c->n); break;
    case 0x50: BRANCH(!c->v); break;
    case 0x70: BRANCH(c->v); break;
    case 0x90: BRANCH(!c->c); break;
    case 0xB0: BRANCH(c->c); break;
    case 0xD0: BRANCH(!c->z); break;
    case 0xF0: BRANCH(c->z); break;

    case 0x00: interrupt(m, 0xFFFE, true); break;
    case 0x20: {
        uint8_t lo = fetch(m);
        idle(m);
        push(m, c->pc >> 8);
        push(m, c->pc & 0xFF);
        uint8_t hi = rd(m, c->pc);
        c->pc = lo | hi << 8;
        break;
    }
    case 0x40: {
        idle(m);
        idle(m);
        set_p(c, pull(m));
        uint8_t lo = pull(m);
        uint8_t hi = pull(m);
        c->pc = lo | hi << 8;
        break;
    }
    case 0x60: {
        idle(m);
        idle(m);
        uint8_t lo = pull(m);
        uint8_t hi = pull(m);
        c->pc = lo | hi << 8;
        idle(m);
        c->pc++;
        break;
    }
    case 0x4C: c->pc = fetch16(m); break;
    case 0x6C: {
        // JMP (ind) does not carry into the pointer's high byte.
        uint16_t p = fetch16(m);
        uint8_t lo = rd(m, p);
        uint8_t hi = rd(m, (p & 0xFF00) | ((p + 1) & 0xFF));
        c->pc = lo | hi << 8;
        break;
    }
    case 0x08: idle(m); push(m, get_p(c, true)); break;
    case 0x28: idle(m); idle(m); set_p(c, pull(m)); break;
    case 0x48: idle(m); push(m, c->a); break;
    case 0x68: idle(m); idle(m); c->a = nz(c, pull(m)); break;

    case 0x18: idle(m); c->c = false; break;
    case 0x38: idle(m); c->c = true; break;
    case 0x58: idle(m); c->i = false; break;
    case 0x78: idle(m); c->i = true; break;
    case 0xB8: idle(m); c->v = false; break;
    case 0xD8: idle(m); c->d = false; break;
    case 0xF8: idle(m); c->d = true; break;
    case 0x88: idle(m); c->y = nz(c, c->y - 1); break;
    case 0xC8: idle(m); c->y = nz(c, c->y + 1); break;
    case 0xCA: idle(m); c->x = nz(c, c->x - 1); break;
    case 0xE8: idle(m); c->x = nz(c, c->x + 1); break;
    case 0x8A: idle(m); c->a = nz(c, c->x); break;
    case 0x98: idle(m); c->a = nz(c, c->y); break;
    case 0xAA: idle(m); c->x = nz(c, c->a); break;
    case 0xA8: idle(m); c->y = nz(c, c->a); break;
    case 0xBA: idle(m); c->x = nz(c, c->s); break;
    case 0x9A: idle(m); c->s = c->x; break;
    case 0xEA: idle(m); break;

    // Undocumented opcodes.
    COMBO_GROUP(0x00, asl(c, v), c->a = nz(c, c->a | v))           // SLO
    COMBO_GROUP(0x20, rol(c, v), c->a = nz(c, c->a & v))           // RLA
    COMBO_GROUP(0x40, lsr(c, v), c->a = nz(c, c->a ^ v))           // SRE
    COMBO_GROUP(0x60, ror(c, v), adc(c, v))                        // RRA
    COMBO_GROUP(0xC0, (uint8_t)(v - 1), cmp(c, c->a, v))           // DCP
    COMBO_GROUP(0xE0, (uint8_t)(v + 1), sbc(c, v))                 // ISC
    // SAX
    case 0x83: IZX(); wr(m, ea, c->a & c->x); break;
    case 0x87: ZP(); wr(m, ea, c->a & c->x); break;
    case 0x8F: ABS(); wr(m, ea, c->a & c->x); break;
    case 0x97: ZPY(); wr(m, ea, c->a & c->x); break;
    // LAX
    case 0xA3: IZX(); c->a = c->x = nz(c, rd(m, ea)); break;
    case 0xA7: ZP(); c->a = c->x = nz(c, rd(m, ea)); break;
    case 0xAF: ABS(); c->a = c->x = nz(c, rd(m, ea)); break;
    case 0xB3: IZY_R(); c->a = c->x = nz(c, rd(m, ea)); break;
    case 0xB7: ZPY(); c->a = c->x = nz(c, rd(m, ea)); break;
    case 0xBF: ABI_R(y); c->a = c->x = nz(c, rd(m, ea)); break;
    case 0xAB: c->a = c->x = nz(c, (c->a | 0xEE) & fetch(m)); break;  // LXA
    case 0x8B: c->a = nz(c, (c->a | 0xEE) & c->x & fetch(m)); break; // ANE
    case 0x0B: case 0x2B: c->a = nz(c, c->a & fetch(m)); c->c = c->n; break;  // ANC
    case 0x4B: c->a = lsr(c, c->a & fetch(m)); break;                         // ALR
    case 0x6B: arr(c, fetch(m)); break;
    case 0xCB: {  // SBX
        uint8_t t = c->a & c->x;
        v = fetch(m);
        c->c = t >= v;
        c->x = nz(c, t - v);
        break;
    }
    case 0xEB: sbc(c, fetch(m)); break;
    case 0xBB: ABI_R(y); c->a = c->x = c->s = nz(c, rd(m, ea) & c->s); break;  // LAS
    case 0x93: {  // SHA (zp),Y
        uint8_t z = fetch(m);
        uint8_t lo = rd(m, z);
        uint8_t hi = rd(m, (uint8_t)(z + 1));
        uint16_t b = lo | hi << 8;
        ea = b + c->y;
        idle(m);
        uint8_t s = c->a & c->x & ((b >> 8) + 1);
        if ((b ^ ea) & 0xFF00) ea = (ea & 0xFF) | s << 8;
        wr(m, ea, s);
        break;
    }
    case 0x9F: STORE_HIGH(c->a & c->x, y); break;               // SHA abs,Y
    case 0x9E: STORE_HIGH(c->x, y); break;                      // SHX
    case 0x9C: STORE_HIGH(c->y, x); break;                      // SHY
    case 0x9B: c->s = c->a & c->x; STORE_HIGH(c->s, y); break;  // TAS
    // NOPs of every length.
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: idle(m); break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: fetch(m); break;
    case 0x04: case 0x44: case 0x64: ZP(); rd(m, ea); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: ZPX(); rd(m, ea); break;
    case 0x0C: ABS(); rd(m, ea); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: ABI_R(x); rd(m, ea); break;
    // KIL: the CPU stops until reset.
    default: c->jammed = true; c->pc--; break;
    }
}

void cpu_run(Machine *m) {
    while (!m->frame_done) step(m);
}

// One instruction (or interrupt entry), for tests.
void cpu_step(Machine *m) { step(m); }
