// Atari 800 XL: a 6502C ("Sally") with ANTIC, GTIA, POKEY and a PIA, 64K
// of RAM and 64K more banked as on the 130XE, the OS ROM, an optional
// cartridge (standard 8K or 16K, MegaCart or SIC!), four disk drives and
// a cassette recorder.
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

// Cartridge banking schemes.
enum { CART_STANDARD, CART_MEGACART, CART_SIC };

// Disk drives, D1-D4. Calls to SIOV are answered by the machine itself,
// at once; programs with their own serial routines reach the drives over
// the serial bus (see `Sio`).
enum { DRIVES = 4, SIOV = 0xE459 };

// CIO, which the built-in OS leaves to the machine: only the cassette.
enum { CIOV = 0xE456 };

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
    uint64_t serin_at;     // cycle the next serial input byte arrives, or ~0
    uint8_t serin;         // the last byte received
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

// A sector of an ATX image as the drive finds it on its track: the
// number in its header, the controller's status reading it (non-zero for
// the errors copy protection depends on), where it passes under the head
// and its data.
typedef struct AtxSector {
    uint8_t number, status;
    uint16_t pos;     // in 8 us units after the index hole, of 26042 a turn
    uint32_t data;    // offset of its data in the file, 0 for none
    int weak;         // offset in the sector where weak bits start, or -1
} AtxSector;

enum { ATX_TRACKS = 40 };

// A disk image in a drive: the sectors of its ATR file or, from an ATX
// file, its tracks as they were recorded, which are read as the disk
// turns. On a double density disk, sectors 1-3 are 128 bytes; most ATR
// images store them so, some in 256-byte slots (`padded`).
typedef struct Disk {
    uint8_t *data;    // the sectors (the whole file for ATX), or NULL for an empty drive
    size_t size;
    int sector_size;  // 128, 256 or 512
    int sectors;
    bool padded;
    bool protect;     // write protected
    int fd;           // the file writes go to, or -1 to keep them in memory
    AtxSector *atx;   // ATX: every track's sectors, or NULL for ATR
    int track[ATX_TRACKS + 1];  // ATX: the first of each track's sectors in `atx`
    int spt;          // ATX: sectors a track
    int head;         // ATX: the track under the head
    uint8_t fdc;      // ATX: the controller's status after the last command
} Disk;

// The disk drives' side of the serial bus. A command frame comes in while
// the command line is low; the drive answers with ACK (or NAK), takes the
// data frame of a write, then sends COMPLETE (or ERROR) and any data.
typedef struct Sio {
    bool command;      // the command line is asserted
    uint8_t in[520];   // the frame coming in
    int nin;
    int want;          // bytes of the data frame awaited (with checksum), or 0
    uint8_t cmd[4];    // the write command it belongs to
    int drive;         // the drive answering, 0-3
    uint8_t out[520];  // the reply
    int nout, sent;
    uint64_t at;       // cycle the next byte of the reply comes in, or ~0
    uint64_t complete; // cycles between ACK and COMPLETE
    // An SIOV call to an ATX disk takes the time the drive would: SIOV
    // waits, interrupts still taken, until `ready`, then returns `status`
    // and `len` bytes of `buf`.
    bool busy;
    uint64_t ready;
    uint8_t status;
    uint8_t buf[256];
    int len;
} Sio;

// A record on a tape: its bytes, the gap of silence before it and the
// rate it was recorded at.
typedef struct TapeRecord {
    size_t offset;
    int length;
    int gap_ms;
    int baud;
} TapeRecord;

// The cassette recorder, playing the records of a .cas file. While the
// motor runs (PIA CA2 low), each byte reaches POKEY's serial input as it
// would from the tape, after its record's gap.
typedef struct Tape {
    uint8_t *data;        // the file, or NULL for no tape
    size_t size;
    TapeRecord *records;
    int nrecords;
    int record, pos;      // the next byte: its record and offset in it
    bool motor;
    uint64_t start, at;   // cycles the next byte starts and has come in, or ~0
} Tape;

// A file open on C: through CIO, and the record it is reading.
typedef struct CasFile {
    int iocb;             // the IOCB it is open on, or -1
    bool eof;
    uint8_t data[128];
    int len, pos;
} CasFile;

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

    // Memory. `rmap`/`wmap` point at each page; NULL is I/O. `amap` is
    // ANTIC's view, which can differ from the CPU's at $4000-$7FFF.
    uint8_t *rmap[256];
    uint8_t *wmap[256];
    uint8_t *amap[256];
    uint8_t sink[256];     // writes to ROM land here
    uint8_t ram[0x10000];
    uint8_t xram[0x10000]; // the 130XE's four extra 16K banks
    uint8_t os[0x4000];    // $C000-$FFFF, self test at $D000-$D7FF
    uint8_t basic[0x2000];
    bool has_basic;
    uint8_t *cart;
    size_t cart_size;
    int cart_type;         // CART_STANDARD, CART_MEGACART or CART_SIC
    uint8_t cart_bank;     // the bank register, $D500
    bool rd5;              // the cartridge shows at $A000-$BFFF

    uint8_t *xex;
    size_t xex_size, xex_offset;
    bool xex_loading;
    uint16_t xex_resume_pc;
    uint8_t xex_resume_s;

    Disk disk[DRIVES];
    Sio sio;
    uint64_t disk_used[DRIVES + 1];  // cycle of each drive's and the tape's last work, 0 for never
    Tape tape;
    CasFile cas;
    bool builtin_os;       // the built-in OS, which has no CIO of its own
    int hold_start;        // frames START stays held for a cassette boot

    Antic antic;
    Gtia gtia;
    Pokey pokey;
    Pia pia;

    // Input, active high: stick bits are up, down, left, right.
    uint8_t stick[2];
    bool trig[2];
    uint8_t consol_keys;   // START 1, SELECT 2, OPTION 4

    uint8_t frame[FRAME_HEIGHT * FRAME_WIDTH];
    // Lines of the frame drawn in high resolution, where the two pixels of
    // a color clock can differ. On the others they are always the same.
    bool frame_hires[FRAME_HEIGHT];
} Machine;

// machine.c
Machine *machine_new(void);
void machine_free(Machine *m);
// `os` is a 16K XL OS ROM, or NULL for the built-in one.
bool machine_set_os(Machine *m, const uint8_t *os, size_t size);
void machine_set_basic(Machine *m, const uint8_t *basic, size_t size);
// Loads a cartridge: a raw 8K or 16K image, or a .car file of a standard,
// MegaCart or SIC! cartridge. Returns an error message, or NULL.
const char *machine_load_cart(Machine *m, const uint8_t *data, size_t size);
void machine_eject_cart(Machine *m);
// Opens a cartridge, .xex, .atr, .atx or .cas by filename and power cycles on
// success. A disk opened while a disk is in D1 replaces it there, and a
// .cas while a tape is in replaces that, without a reset. Writes to a disk go to the file at `path` (or stay in memory when
// it is NULL), unless its header marks it write protected or the file
// cannot be written.
const char *machine_load_file(Machine *m, const char *path, const uint8_t *data, size_t size);
// Removes everything, inserts `n` .atr or .atx images into D1 onwards and power
// cycles. Leaves the machine as it was on failure.
const char *machine_load_disks(Machine *m, int n, const char *const paths[], const uint8_t *const data[],
                               const size_t sizes[]);
void machine_eject_file(Machine *m);
// Answers a call to SIOV for a disk drive or the cassette, at an
// instruction boundary: SIO_DONE when it has returned, SIO_WAIT while a
// drive is still at work (the CPU idles a cycle), SIO_NOT_MINE to run the
// code there.
enum { SIO_NOT_MINE, SIO_DONE, SIO_WAIT };
int machine_sio(Machine *m);
// Answers a call to CIOV of the built-in OS for C:, at an instruction
// boundary. Other devices are not there.
bool machine_cio(Machine *m);
// The serial bus: POKEY sends a byte, and takes the next byte of a reply
// or of the tape when `pokey.serin_at` comes (which this schedules).
void machine_serout(Machine *m, uint8_t v);
uint8_t machine_serin(Machine *m);
// The level of the serial input line, as SKSTAT shows it: the tape's bits
// as they play, high when idle.
bool machine_serial_line(const Machine *m);
// The drives at work in the last eighth of a second, a bit for each of
// D1-D4 and bit 4 for the cassette: what their busy lights show. An SIOV
// call takes no time, so the light stays on a little after it.
unsigned machine_disk_activity(const Machine *m);
// At an instruction boundary, starts the loader or resumes it after INITAD.
bool machine_step_xex(Machine *m);
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
// ANTIC reads memory as it sees it, without I/O.
static inline uint8_t antic_peek(const Machine *m, uint16_t addr) {
    const uint8_t *p = m->amap[addr >> 8];
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
