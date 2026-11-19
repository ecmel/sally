// Disk boot, DSKINV and SIOV on ATR images, through the same entry points
// as the apps, and the drives on the serial bus.
#include "asm6502.h"
#include "machine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Boots, then reads and writes sectors through DSKINV and stores each
// status at $3001 onwards. DOSINI counts at $3000.
static const char *const boot =
    "DOSVEC = $0A\n"
    "DSCTLN = $02D5\n"
    "DUNIT  = $0301\n"
    "DCOMND = $0302\n"
    "DBUFLO = $0304\n"
    "DBUFHI = $0305\n"
    "DAUX1  = $030A\n"
    "DAUX2  = $030B\n"
    "DSKINV = $E453\n"
    "        .org $0700\n"
    "        .byte 0, 2\n"
    "        .word $0700, dosini\n"
    "        lda #<main\n"
    "        sta DOSVEC\n"
    "        lda #>main\n"
    "        sta DOSVEC+1\n"
    "        clc\n"
    "        rts\n"
    "dosini: inc $3000\n"
    "        rts\n"
    "; Unit in A, command in X, buffer page in Y; the sector follows the JSR.\n"
    "io:     sta DUNIT\n"
    "        stx DCOMND\n"
    "        sty DBUFHI\n"
    "        lda #0\n"
    "        sta DBUFLO\n"
    "        pla\n"
    "        sta $80\n"
    "        pla\n"
    "        sta $81\n"
    "        ldy #1\n"
    "        lda ($80),y\n"
    "        sta DAUX1\n"
    "        iny\n"
    "        lda ($80),y\n"
    "        sta DAUX2\n"
    "        lda $80\n"
    "        clc\n"
    "        adc #2\n"
    "        tax\n"
    "        lda $81\n"
    "        adc #0\n"
    "        pha\n"
    "        txa\n"
    "        pha\n"
    "        jmp DSKINV\n"
    "main:   lda #1\n"
    "        ldx #'R'\n"
    "        ldy #$40\n"
    "        jsr io\n"
    "        .word 720\n"
    "        sty $3001\n"
    "        inc $4000\n"
    "        lda #1\n"
    "        ldx #'W'\n"
    "        ldy #$40\n"
    "        jsr io\n"
    "        .word 719\n"
    "        sty $3002\n"
    "        lda #2\n"
    "        ldx #'R'\n"
    "        ldy #$41\n"
    "        jsr io\n"
    "        .word 1\n"
    "        sty $3003\n"
    "        lda #3\n"
    "        ldx #'S'\n"
    "        ldy #$40\n"
    "        jsr io\n"
    "        .word 0\n"
    "        sty $3004\n"
    "        lda #1\n"
    "        ldx #'R'\n"
    "        ldy #$40\n"
    "        jsr io\n"
    "        .word 721\n"
    "        sty $3005\n"
    "        lda #0\n"
    "        sta DSCTLN\n"
    "        lda #1\n"
    "        sta DSCTLN+1\n"
    "        lda #2\n"
    "        ldx #'R'\n"
    "        ldy #$42\n"
    "        jsr io\n"
    "        .word 4\n"
    "        sty $3006\n"
    "loop:   jmp loop\n";

// An ATR of `sectors` sectors of `size` bytes, sectors 1-3 of 128 on a
// double density disk. Each sector is filled with its number, low byte.
static uint8_t *make_atr(int sectors, int size, bool protect, size_t *len) {
    size_t data = size == 256 ? 384 + (size_t)(sectors - 3) * 256 : (size_t)sectors * size;
    uint8_t *atr = calloc(1, 16 + data);
    size_t paragraphs = data / 16;
    atr[0] = 0x96;
    atr[1] = 0x02;
    atr[2] = paragraphs & 0xFF;
    atr[3] = paragraphs >> 8 & 0xFF;
    atr[4] = size & 0xFF;
    atr[5] = size >> 8;
    atr[6] = paragraphs >> 16;
    atr[15] = protect;
    for (int n = 1; n <= sectors; n++) {
        int len = size == 256 && n <= 3 ? 128 : size;
        size_t at = size == 256 && n > 3 ? 384 + (size_t)(n - 4) * 256 : (size_t)(n - 1) * (n <= 3 && size == 256 ? 128 : size);
        memset(atr + 16 + at, n, len);
    }
    *len = 16 + data;
    return atr;
}

static uint8_t *boot_atr(bool protect, size_t *len) {
    uint8_t *atr = make_atr(720, 128, protect, len);
    uint8_t code[256];
    char err[128];
    if (!asm6502(boot, code, 0x0700, sizeof code, err, sizeof err)) {
        fprintf(stderr, "boot code: %s\n", err);
        exit(1);
    }
    memcpy(atr + 16, code, sizeof code);
    return atr;
}

static void run(Machine *m, int frames) {
    for (int i = 0; i < frames; i++) machine_run_frame(m);
}

static void check_booted(Machine *m, uint8_t write_status) {
    assert(m->ram[0x3000] == 1);
    assert(m->ram[0x3001] == 0x01 && m->ram[0x4000] == (720 & 0xFF) + 1);
    assert(m->ram[0x3002] == write_status);
    assert(m->ram[0x3003] == 0x01 && m->ram[0x4100] == 1 && m->ram[0x417F] == 1 && m->ram[0x4180] == 0);
    assert(m->ram[0x3004] == 0x8A);
    assert(m->ram[0x3005] == 0x8B);
    assert(m->ram[0x3006] == 0x01 && m->ram[0x4200] == 4 && m->ram[0x42FF] == 4);
}

// The serial bus, driven as a program with its own serial routine would:
// a command frame under the command line, then the drive's reply byte by
// byte through SERIN and its interrupt. Returns the bytes received.
static uint8_t bus_checksum(const uint8_t *p, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; i++) {
        sum += p[i];
        sum = (sum & 0xFF) + (sum >> 8);
    }
    return (uint8_t)sum;
}

static void bus_send(Machine *m, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        io_write(m, 0xD20D, p[i]);
        m->clock += 1000;
    }
}

static int bus_receive(Machine *m, uint8_t *out, int max) {
    int n = 0;
    for (int t = 0; t < 400000 && n < max; t += 50) {
        m->clock += 50;
        if (!(io_read(m, 0xD20E) & 0x20)) {
            out[n++] = io_read(m, 0xD20D);
            io_write(m, 0xD20E, 0x00);
            io_write(m, 0xD20E, 0x20);
        }
    }
    return n;
}

static int bus_command(Machine *m, uint8_t dev, uint8_t cmd, int aux, uint8_t *out, int max) {
    uint8_t frame[5] = {dev, cmd, (uint8_t)aux, (uint8_t)(aux >> 8)};
    frame[4] = bus_checksum(frame, 4);
    io_write(m, 0xD20E, 0x20);
    io_write(m, 0xD303, 0x34);
    bus_send(m, frame, 5);
    io_write(m, 0xD303, 0x3C);
    return bus_receive(m, out, max);
}

static void check_bus(Machine *m) {
    uint8_t r[600];
    // A read: ACK, COMPLETE, the sector and its checksum.
    assert(bus_command(m, 0x31, 'R', 2, r, sizeof r) == 131);
    assert(r[0] == 'A' && r[1] == 'C' && r[2] == m->disk[0].data[128] && r[130] == bus_checksum(r + 2, 128));
    // A 256-byte sector of D2.
    assert(bus_command(m, 0x32, 'R', 10, r, sizeof r) == 259);
    assert(r[1] == 'C' && r[2] == 10 && r[257] == 10);
    // A write: ACK, the data frame, ACK and COMPLETE. D2 has no file to
    // write to, so it is protected and answers ERROR.
    uint8_t data[129];
    memset(data, 0x5A, 128);
    data[128] = bus_checksum(data, 128);
    assert(bus_command(m, 0x31, 'W', 9, r, 1) == 1 && r[0] == 'A');
    bus_send(m, data, sizeof data);
    assert(bus_receive(m, r, sizeof r) == 2 && r[0] == 'A' && r[1] == 'C');
    assert(m->disk[0].data[8 * 128] == 0x5A && m->disk[0].data[9 * 128 - 1] == 0x5A);
    uint8_t data2[257];
    memset(data2, 0x5A, 256);
    data2[256] = bus_checksum(data2, 256);
    assert(bus_command(m, 0x32, 'P', 9, r, 1) == 1 && r[0] == 'A');
    bus_send(m, data2, sizeof data2);
    assert(bus_receive(m, r, sizeof r) == 2 && r[0] == 'A' && r[1] == 'E');
    // No such sector: NAK. No such drive: silence.
    assert(bus_command(m, 0x31, 'R', 0, r, sizeof r) == 1 && r[0] == 'N');
    assert(bus_command(m, 0x33, 'R', 1, r, sizeof r) == 0);
}

int main(void) {
    char path[] = "/tmp/sally-atrtest-XXXXXX.atr";
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    size_t blen, dlen;
    uint8_t *disk1 = boot_atr(false, &blen);
    uint8_t *disk2 = make_atr(720, 256, false, &dlen);
    assert(write(fd, disk1, blen) == (ssize_t)blen);
    close(fd);

    Machine *m = machine_new();
    assert(m);
    const char *paths[] = {path, "two.ATR"};
    const uint8_t *data[] = {disk1, disk2};
    const size_t sizes[] = {blen, dlen};
    assert(!machine_load_disks(m, 2, paths, data, sizes));
    assert(m->disk[1].sectors == 720 && !m->disk[1].padded);
    run(m, 3);
    check_booted(m, 0x01);
    // Both drives were at work; their lights go out soon after.
    assert(machine_disk_activity(m) == 3);
    run(m, 10);
    assert(machine_disk_activity(m) == 0);

    // The write reached the file.
    FILE *f = fopen(path, "rb");
    uint8_t sector[128];
    assert(f && fseek(f, 16 + 718 * 128, SEEK_SET) == 0 && fread(sector, 1, 128, f) == 128);
    fclose(f);
    assert(sector[0] == (720 & 0xFF) + 1 && sector[1] == (720 & 0xFF));

    check_bus(m);

    // RESET boots again, with the disks still in.
    machine_warm_reset(m);
    run(m, 3);
    check_booted(m, 0x01);

    // The next disk goes into D1 without a reset.
    uint64_t clock = m->clock;
    assert(!machine_load_file(m, "next.atr", disk2, dlen));
    assert(m->clock == clock && m->ram[0x3000] == 1 && m->disk[0].sector_size == 256 && m->disk[1].data);
    assert(machine_load_file(m, "bad.atr", disk1 + 1, blen - 1));
    assert(m->disk[0].sector_size == 256);

    // A disk whose header protects it refuses writes, and its file is left alone.
    size_t plen;
    uint8_t *protected = boot_atr(true, &plen);
    const char *ppaths[] = {"protected.atr", "two.atr"};
    const uint8_t *pdata[] = {protected, disk2};
    assert(!machine_load_disks(m, 2, ppaths, pdata, sizes));
    run(m, 3);
    check_booted(m, 0x90);

    // Opening a cartridge with a disk, or too many disks, changes nothing.
    const char *mixed[] = {"one.atr", "game.rom"};
    assert(machine_load_disks(m, 2, mixed, pdata, sizes));
    assert(m->disk[0].data && m->disk[0].protect);

    // A cartridge ejects the disks; without them nothing boots.
    const uint8_t cart[0x2000] = {
        0x60, [0x1FFA] = 0x00, [0x1FFB] = 0xA0, [0x1FFE] = 0x00, [0x1FFF] = 0xA0,
    };
    assert(!machine_load_file(m, "game.rom", cart, sizeof cart));
    assert(!m->disk[0].data && !m->disk[1].data);
    machine_eject_file(m);
    machine_cold_reset(m);
    run(m, 3);
    assert(m->ram[0x3000] == 0 && m->ram[0x3001] == 0);

    machine_free(m);
    unlink(path);
    free(disk1);
    free(disk2);
    free(protected);
    puts("ATR disks: pass");
    return 0;
}
