// Atari 800 XL: a 6502C ("Sally") with ANTIC, GTIA, POKEY and a PIA, 64K
// of RAM, the OS ROM and an optional cartridge.
//
// Time is counted in CPU cycles. A scanline is 114 cycles, an NTSC frame
// 262 lines. ANTIC takes cycles from the CPU for its DMA; `dma` marks them
// for the current line, and the CPU waits for a free one before each bus
// access. GTIA draws a line in pieces: before a register write takes
// effect, the line is drawn up to where the beam is.

#ifndef SALLY_MACHINE_H
#define SALLY_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CYCLES_PER_LINE = 114,
    LINES_PER_FRAME = 262,
    // The part of the picture kept in the frame buffer: lines 8-247 and
    // color clocks 32-223, two pixels per color clock.
    FRAME_TOP = 8,
    FRAME_BOTTOM = 248,
    FRAME_HEIGHT = FRAME_BOTTOM - FRAME_TOP,
    FRAME_LEFT = 32,
    FRAME_CLOCKS = 192,
    FRAME_WIDTH = FRAME_CLOCKS * 2,
    // Color clocks in a line.
    LINE_CLOCKS = 228,
    AUDIO_MAX = 4096,
};

#define CPU_HZ 1789772.5

typedef struct Cpu {
    uint16_t pc;
    uint8_t a, x, y, s;
    bool n, v, d, i, z, c;
    bool jammed;
} Cpu;

typedef struct Antic {
    uint8_t dmactl, chactl, hscrol, vscrol, pmbase, chbase, nmien, nmist;
    uint16_t dlist;  // display list counter
    uint16_t scan;   // memory scan counter
    uint8_t ir;      // current display list instruction
    uint8_t mode;    // its mode, 0 for blank lines
    uint8_t left;    // scanlines left in the mode line
    uint8_t row;     // row within the mode line (4 bits)
    uint8_t nbytes;  // playfield bytes in the line buffer
    bool vscrolled;  // the last mode line had vertical scrolling
    bool waiting;    // JVB: blank until vertical blank
    uint8_t linebuf[64];
    // ANTIC's output for the current line, per color clock: 0 background,
    // 1-4 PF0-PF3. On a high resolution line (modes 2, 3 and F) `hires`
    // is set and `hi` holds each color clock's two half-clock pixels.
    uint8_t pf[LINE_CLOCKS];
    uint8_t hi[LINE_CLOCKS];
    bool hires;
    int pf_start;           // color clock of the first byte of data
    int pf_left, pf_right;  // color clocks of the data that is shown
} Antic;

typedef struct Gtia {
    uint8_t hpos[8];   // players 0-3, missiles 0-3
    uint8_t sizep[4], sizem;
    uint8_t grafp[4], grafm;
    uint8_t col[9];    // COLPM0-3, COLPF0-3, COLBK as written
    uint8_t prior, vdelay, gractl, consol_out;
    uint8_t coll[16];  // M0PF-M3PF, P0PF-P3PF, M0PL-M3PL, P0PL-P3PL
    uint8_t trig_latch;
    int cc;            // next color clock to draw on the current line
} Gtia;

typedef struct Pokey {
    uint8_t audf[4], audc[4], audctl, skctl, irqen, irqst, kbcode;
    uint64_t next[4];      // cycle each channel's divider next runs out
    bool out[4], hp[2];    // channel outputs, high-pass flip-flops
    uint64_t t;            // cycles synthesized so far
    uint64_t irq_next;     // cycle of the next timer interrupt, or ~0
    uint64_t serout_at;    // cycle the serial output byte is sent, or ~0
    int level;             // summed channel volumes right now
    double sample_next;    // cycle of the next audio sample boundary
    double cps;            // cycles per audio sample
    double acc;            // level integrated since the last sample
    float dc_x, dc_y;      // DC blocker state
    float audio[AUDIO_MAX];
    int naudio;
    bool key_down, shift_down;
} Pokey;

typedef struct Pia {
    uint8_t ora, ddra, pactl, orb, ddrb, pbctl;
} Pia;

typedef struct Machine {
    Cpu cpu;

    // Beam and clock.
    int x;                 // cycle within the line
    int line;
    uint64_t clock;        // cycles since power on
    uint64_t cpu_cycles;   // cycles the CPU has used
    uint8_t dma[CYCLES_PER_LINE + 1];  // cycles ANTIC takes on this line
    bool wsync;            // the CPU waits for the end of the line
    bool frame_done;

    // Interrupts.
    bool nmi;              // an NMI is pending
    uint64_t nmi_at;       // when it was raised
    uint64_t last_access;  // cycle of the CPU's last bus access
    bool irq;              // POKEY's IRQ line

    // Memory. `rmap`/`wmap` point at each page; NULL is I/O.
    uint8_t *rmap[256];
    uint8_t *wmap[256];
    uint8_t sink[256];     // writes to ROM land here
    uint8_t ram[0x10000];
    uint8_t os[0x4000];    // $C000-$FFFF, self test at $D000-$D7FF
    uint8_t basic[0x2000];
    bool has_basic;
    uint8_t *cart;
    size_t cart_size;

    Antic antic;
    Gtia gtia;
    Pokey pokey;
    Pia pia;

    // Input, active high: stick bits are up, down, left, right.
    uint8_t stick[2];
    bool trig[2];
    uint8_t consol_keys;   // START 1, SELECT 2, OPTION 4

    uint8_t frame[FRAME_HEIGHT * FRAME_WIDTH];
} Machine;

// machine.c
Machine *machine_new(void);
void machine_free(Machine *m);
// `os` is a 16K XL OS ROM, or NULL for the built-in one.
bool machine_set_os(Machine *m, const uint8_t *os, size_t size);
void machine_set_basic(Machine *m, const uint8_t *basic, size_t size);
// Loads a cartridge (raw 8K or 16K, or a .car file). Returns an error
// message, or NULL.
const char *machine_load_cart(Machine *m, const uint8_t *data, size_t size);
void machine_eject_cart(Machine *m);
void machine_cold_reset(Machine *m);
void machine_warm_reset(Machine *m);
// Runs until the beam reaches line 248, where vertical blank begins and
// the frame buffer holds a whole frame.
void machine_run_frame(Machine *m);
void machine_key(Machine *m, uint8_t code, bool down);
void machine_break_key(Machine *m);
void machine_update_memory_map(Machine *m);
void machine_end_line(Machine *m);
void machine_wait_wsync(Machine *m);
uint8_t io_read(Machine *m, uint16_t addr);
void io_write(Machine *m, uint16_t addr, uint8_t v);
// ANTIC reads memory as the CPU sees it, without I/O.
static inline uint8_t antic_peek(const Machine *m, uint16_t addr) {
    const uint8_t *p = m->rmap[addr >> 8];
    return p ? p[addr & 0xFF] : 0xFF;
}

// cpu.c
void cpu_reset(Machine *m);
void cpu_run(Machine *m);
void cpu_step(Machine *m);

// antic.c
void antic_start_line(Machine *m);
uint8_t antic_read(Machine *m, uint8_t reg);
void antic_write(Machine *m, uint8_t reg, uint8_t v);

// gtia.c
void gtia_init(void);
void gtia_render(Machine *m, int cc);
void gtia_pm_dma(Machine *m);
uint8_t gtia_read(Machine *m, uint8_t reg);
void gtia_write(Machine *m, uint8_t reg, uint8_t v);
// RGB (0xRRGGBB) for each of the 256 color values.
extern uint32_t gtia_palette[256];

// pokey.c
void pokey_init(void);
void pokey_reset(Machine *m);
void pokey_sync(Machine *m);
// Recomputes the output level after the console speaker changes.
void pokey_refresh(Machine *m);
void pokey_set_rate(Machine *m, double sample_rate);
uint8_t pokey_read(Machine *m, uint8_t reg);
void pokey_write(Machine *m, uint8_t reg, uint8_t v);
void pokey_key(Machine *m, uint8_t code, bool down);
void pokey_break(Machine *m);

// os.c: the built-in OS, assembled into a 16K image.
bool os_build(uint8_t rom[0x4000], char *err, size_t errlen);

#endif
