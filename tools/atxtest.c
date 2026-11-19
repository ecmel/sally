// ATX disks: sectors read as the disk turns, with the duplicates, errors,
// missing and weak sectors of copy protection, through SIOV and the
// serial bus.
#include "machine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t number, status;
    uint16_t pos;
    uint8_t fill;
    int weak;  // offset of weak bits, or -1
} Sec;

static void put16(uint8_t *p, int v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8 & 0xFF;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

// A track record: header, sector list, the sectors' data, weak sector
// chunks and the terminator.
static size_t track(uint8_t *out, int t, const Sec *s, int n) {
    int weak = 0;
    for (int i = 0; i < n; i++) weak += s[i].weak >= 0;
    size_t list = 32, data = list + 8 + 8 * n, weaks = data + 8 + 128 * n, size = weaks + 8 * weak + 8;
    memset(out, 0, size);
    put32(out, (uint32_t)size);
    out[8] = t;
    put16(out + 10, n);
    put32(out + 20, 32);
    put32(out + list, 8 + 8 * n);
    out[list + 4] = 1;
    put32(out + data, 8 + 128 * n);
    uint8_t *w = out + weaks;
    for (int i = 0; i < n; i++) {
        uint8_t *e = out + list + 8 + 8 * i;
        e[0] = s[i].number;
        e[1] = s[i].status;
        put16(e + 2, s[i].pos);
        put32(e + 4, (uint32_t)(data + 8 + 128 * i));
        memset(out + data + 8 + 128 * i, s[i].fill, 128);
        if (s[i].weak >= 0) {
            put32(w, 8);
            w[4] = 0x10;
            w[5] = i;
            put16(w + 6, s[i].weak);
            w += 8;
        }
    }
    return size;
}

// Track 0 is plain: sectors 1-18, each filled with its number. On track 1,
// sector 19 is there twice ($AA early in the turn, $BB late), sector 20
// has lost data and a CRC error, 21 is missing and 22 has weak bits from
// byte 64.
static uint8_t *make_atx(size_t *len) {
    uint8_t *atx = calloc(1, 16384);
    memcpy(atx, "AT8X", 4);
    put16(atx + 4, 1);
    put16(atx + 6, 1);
    put32(atx + 28, 48);
    size_t at = 48;
    Sec s[20];
    for (int i = 0; i < 18; i++) s[i] = (Sec){(uint8_t)(i + 1), 0, (uint16_t)(i * 1400), (uint8_t)(i + 1), -1};
    at += track(atx + at, 0, s, 18);
    int n = 0;
    s[n++] = (Sec){1, 0, 1000, 0xAA, -1};
    s[n++] = (Sec){1, 0, 14000, 0xBB, -1};
    s[n++] = (Sec){2, 0x0C, 3000, 0x20, -1};
    s[n++] = (Sec){4, 0, 5000, 0x22, 64};
    for (int i = 5; i <= 18; i++) s[n++] = (Sec){(uint8_t)i, 0, (uint16_t)(i * 1400), (uint8_t)(18 + i), -1};
    at += track(atx + at, 1, s, n);
    put32(atx + 32, (uint32_t)at);
    *len = at;
    return atx;
}

// Calls SIOV for D1 as a program would, from $0600, and returns the
// status; `*cycles` is how long it took.
static uint8_t sio(Machine *m, uint8_t cmd, int sector, uint64_t *cycles) {
    static const uint8_t call[] = {0x20, 0x59, 0xE4, 0x4C, 0x03, 0x06};  // JSR SIOV; JMP *
    memcpy(m->ram + 0x0600, call, sizeof call);
    m->ram[0x0300] = 0x31;
    m->ram[0x0301] = 1;
    m->ram[0x0302] = cmd;
    m->ram[0x0304] = 0x00;
    m->ram[0x0305] = 0x40;
    m->ram[0x0308] = cmd == 'S' ? 4 : 128;
    m->ram[0x0309] = 0;
    m->ram[0x030A] = sector & 0xFF;
    m->ram[0x030B] = sector >> 8;
    m->cpu.pc = 0x0600;
    uint64_t start = m->clock;
    while (m->cpu.pc != 0x0603) cpu_step(m);
    if (cycles) *cycles = m->clock - start;
    return m->ram[0x0303];
}

static uint8_t checksum(const uint8_t *p, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; i++) {
        sum += p[i];
        sum = (sum & 0xFF) + (sum >> 8);
    }
    return (uint8_t)sum;
}

// A read over the serial bus: the command frame under the command line,
// then the reply through SERIN.
static int bus_read(Machine *m, int sector, uint8_t *out, int max) {
    uint8_t frame[5] = {0x31, 'R', (uint8_t)sector, (uint8_t)(sector >> 8)};
    frame[4] = checksum(frame, 4);
    io_write(m, 0xD20E, 0x20);
    io_write(m, 0xD303, 0x34);
    for (int i = 0; i < 5; i++) {
        io_write(m, 0xD20D, frame[i]);
        m->clock += 1000;
    }
    io_write(m, 0xD303, 0x3C);
    int n = 0;
    for (int t = 0; t < 3000000 && n < max; t += 50) {
        m->clock += 50;
        if (!(io_read(m, 0xD20E) & 0x20)) {
            out[n++] = io_read(m, 0xD20D);
            io_write(m, 0xD20E, 0x00);
            io_write(m, 0xD20E, 0x20);
        }
    }
    return n;
}

int main(void) {
    const uint64_t turn = (uint64_t)(CPU_HZ * 60 / 288);
    size_t len;
    uint8_t *atx = make_atx(&len);
    Machine *m = machine_new();
    assert(m);
    const char *path = "game.atx";
    assert(!machine_load_disks(m, 1, &path, (const uint8_t *const *)&atx, &len));
    assert(m->disk[0].sectors == 720 && m->disk[0].protect && m->disk[0].atx);

    // A plain sector takes the time of the bus and of the disk turning.
    uint64_t cycles;
    assert(sio(m, 'R', 5, &cycles) == 0x01 && m->ram[0x4000] == 5 && m->ram[0x407F] == 5);
    assert(cycles > 130 * 932 && cycles < 130 * 932 + 2 * turn);
    // SIO leaves the data frame's checksum in CHKSUM, the status in STATUS.
    assert(m->ram[0x31] == checksum(m->ram + 0x4000, 128) && m->ram[0x30] == 0x01);

    // Which copy of sector 19 comes back depends on when it is asked for.
    bool aa = false, bb = false;
    for (int i = 0; i < 16; i++) {
        m->clock += turn / 16 + 977;
        assert(sio(m, 'R', 19, NULL) == 0x01);
        aa |= m->ram[0x4000] == 0xAA;
        bb |= m->ram[0x4000] == 0xBB;
        assert(m->ram[0x4000] == 0xAA || m->ram[0x4000] == 0xBB);
    }
    assert(aa && bb);

    // An error: the data comes back with it, and the status command gives
    // the controller's status, inverted, DRQ set by the lost data.
    assert(sio(m, 'R', 20, NULL) == 0x90 && m->ram[0x4000] == 0x20);
    assert(sio(m, 'S', 0, NULL) == 0x01 && m->ram[0x4001] == 0xF1);

    // A missing sector, after two turns, twice.
    assert(sio(m, 'R', 21, &cycles) == 0x90 && cycles > 4 * turn);
    assert(sio(m, 'S', 0, NULL) == 0x01 && m->ram[0x4001] == 0xEF);

    // Weak bits read differently; the bytes before them do not.
    uint8_t first[128];
    assert(sio(m, 'R', 22, NULL) == 0x01);
    memcpy(first, m->ram + 0x4000, 128);
    assert(sio(m, 'R', 22, NULL) == 0x01);
    assert(memcmp(first, m->ram + 0x4000, 64) == 0 && first[0] == 0x22);
    assert(memcmp(first + 64, m->ram + 0x4040, 64) != 0);

    // ATX disks are write protected.
    assert(sio(m, 'W', 5, NULL) == 0x90);

    // Over the bus: ACK, then COMPLETE once the sector has been found.
    uint8_t r[200];
    assert(bus_read(m, 19, r, 131) == 131);
    assert(r[0] == 'A' && r[1] == 'C' && (r[2] == 0xAA || r[2] == 0xBB) && r[130] == checksum(r + 2, 128));
    assert(bus_read(m, 20, r, 131) == 131 && r[1] == 'E' && r[2] == 0x20);

    machine_free(m);
    free(atx);
    puts("ATX disks: pass");
    return 0;
}
