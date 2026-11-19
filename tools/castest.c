// Cassette boot, SIOV and CIO reads and the tape playing into POKEY, on
// CAS images, through the same entry points as the apps.
#include "asm6502.h"
#include "machine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Two boot records. CASINI counts at $3000; the program reads the next
// record into $4000 through SIOV and stores the status at $3001.
static const char *const boot =
    "DOSVEC = $0A\n"
    "DDEVIC = $0300\n"
    "DUNIT  = $0301\n"
    "DCOMND = $0302\n"
    "DSTATS = $0303\n"
    "DBUFLO = $0304\n"
    "DBUFHI = $0305\n"
    "DBYTLO = $0308\n"
    "DBYTHI = $0309\n"
    "SIOV   = $E459\n"
    "        .org $0700\n"
    "        .byte 0, 2\n"
    "        .word $0700, casini\n"
    "        lda #<main\n"
    "        sta DOSVEC\n"
    "        lda #>main\n"
    "        sta DOSVEC+1\n"
    "        clc\n"
    "        rts\n"
    "casini: inc $3000\n"
    "        rts\n"
    "main:   lda #$60\n"
    "        sta DDEVIC\n"
    "        lda #0\n"
    "        sta DUNIT\n"
    "        sta DBUFLO\n"
    "        sta DBYTHI\n"
    "        lda #$40\n"
    "        sta DBUFHI\n"
    "        sta DSTATS\n"
    "        lda #'R'\n"
    "        sta DCOMND\n"
    "        lda #131\n"
    "        sta DBYTLO\n"
    "        jsr SIOV\n"
    "        sty $3001\n"
    "loop:   jmp loop\n"
    "        .org $07FF\n"
    "        .byte $EA\n";

// Two boot records that read the file after it through CIO, on IOCB 1:
// 100 bytes, one byte in A, then the rest up to 200. Each status, length
// and byte read goes to $3010 onwards.
static const char *const cioboot =
    "DOSVEC = $0A\n"
    "ICCOM  = $0342\n"
    "ICSTA  = $0343\n"
    "ICBAL  = $0344\n"
    "ICBAH  = $0345\n"
    "ICBLL  = $0348\n"
    "ICBLH  = $0349\n"
    "ICAX1  = $034A\n"
    "CIOV   = $E456\n"
    "        .org $0700\n"
    "        .byte 0, 2\n"
    "        .word $0700, return\n"
    "        lda #<main\n"
    "        sta DOSVEC\n"
    "        lda #>main\n"
    "        sta DOSVEC+1\n"
    "        clc\n"
    "return: rts\n"
    "main:   ldx #$10\n"
    "        lda #<kname\n"
    "        ldy #>kname\n"
    "        jsr open\n"
    "        sty $3010\n"
    "        lda #<cname\n"
    "        ldy #>cname\n"
    "        jsr open\n"
    "        sty $3011\n"
    "        lda #100\n"
    "        jsr get\n"
    "        sty $3012\n"
    "        lda ICBLL,x\n"
    "        sta $3013\n"
    "        lda #0\n"
    "        jsr get\n"
    "        sty $3014\n"
    "        sta $3015\n"
    "        lda #200\n"
    "        jsr get\n"
    "        sty $3016\n"
    "        lda ICBLL,x\n"
    "        sta $3017\n"
    "        lda #12\n"
    "        sta ICCOM,x\n"
    "        jsr CIOV\n"
    "        sty $3018\n"
    "loop:   jmp loop\n"
    "open:   sta ICBAL,x\n"
    "        tya\n"
    "        sta ICBAH,x\n"
    "        lda #3\n"
    "        sta ICCOM,x\n"
    "        lda #4\n"
    "        sta ICAX1,x\n"
    "        jmp CIOV\n"
    "get:    sta ICBLL,x\n"
    "        lda #0\n"
    "        sta ICBLH,x\n"
    "        sta ICBAL,x\n"
    "        lda #$50\n"
    "        sta ICBAH,x\n"
    "        lda #7\n"
    "        sta ICCOM,x\n"
    "        jmp CIOV\n"
    "kname:  .byte 'K', ':', $9B\n"
    "cname:  .byte 'C', ':', $9B\n"
    "        .org $07FF\n"
    "        .byte 0\n";

static uint8_t checksum(const uint8_t *p, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; i++) {
        sum += p[i];
        sum = (sum & 0xFF) + (sum >> 8);
    }
    return (uint8_t)sum;
}

typedef struct {
    uint8_t *data;
    size_t size;
} Buf;

static void chunk(Buf *b, const char *type, int aux, const uint8_t *data, int len) {
    b->data = realloc(b->data, b->size + 8 + len);
    uint8_t *p = b->data + b->size;
    memcpy(p, type, 4);
    p[4] = len & 0xFF;
    p[5] = len >> 8;
    p[6] = aux & 0xFF;
    p[7] = aux >> 8;
    if (len) memcpy(p + 8, data, len);
    b->size += 8 + len;
}

// A standard record: speed marks, control byte, 128 bytes and the checksum.
static void record(Buf *b, int gap, uint8_t control, const uint8_t data[128], uint8_t out[132]) {
    out[0] = out[1] = 0x55;
    out[2] = control;
    memcpy(out + 3, data, 128);
    out[131] = checksum(out, 131);
    chunk(b, "data", gap, out, 132);
}

static void check_booted(Machine *m) {
    assert(m->ram[0x3000] == 1);
    assert(m->ram[0x09] == 2);  // BOOTQ: booted from the cassette
    // The init address went to DOSINI, as on a disk, and moved to CASINI.
    assert(m->ram[0x0C] == m->ram[0x02] && m->ram[0x0D] == m->ram[0x03] && m->ram[0x0D] == 0x07);
    assert(m->ram[0x3001] == 0x01 && m->ram[0x4002] == 0xFC && m->ram[0x4003] == 0xA5 &&
           m->ram[0x4082] == (uint8_t)(0xA5 + 127));
    // START is let go once the OS has read the tape.
    assert(io_read(m, 0xD01F) == 0x07);
}

int main(void) {
    uint8_t code[256];
    char err[128];
    if (!asm6502(boot, code, 0x0700, sizeof code, err, sizeof err)) {
        fprintf(stderr, "boot code: %s\n", err);
        return 1;
    }
    Buf cas = {0};
    chunk(&cas, "FUJI", 0, (const uint8_t *)"test", 4);
    chunk(&cas, "baud", 600, NULL, 0);
    uint8_t r1[132], r2[132], r3[132], r4[132], r5[132], data[128];
    record(&cas, 2000, 0xFC, code, r1);
    record(&cas, 250, 0xFC, code + 128, r2);
    for (int i = 0; i < 128; i++) data[i] = 0xA5 + i;
    record(&cas, 250, 0xFC, data, r3);
    for (int i = 0; i < 128; i++) data[i] = i * 3;
    record(&cas, 250, 0xFC, data, r4);
    memset(data, 0, sizeof data);
    record(&cas, 250, 0xFE, data, r5);

    Machine *m = machine_new();
    assert(m);
    assert(!machine_load_file(m, "game.cas", cas.data, cas.size));
    assert(m->tape.nrecords == 5 && m->tape.records[0].baud == 600 && m->tape.records[0].gap_ms == 2000);
    for (int i = 0; i < 3; i++) machine_run_frame(m);
    check_booted(m);
    assert(machine_disk_activity(m) == 0x10);

    // With the motor on, record 4 plays into POKEY after its gap, 600 baud.
    io_write(m, 0xD20E, 0x20);
    io_write(m, 0xD302, 0x34);
    uint64_t on = m->clock;
    uint8_t got[132];
    int n = 0, low = 0;
    uint64_t first = 0;
    for (int t = 0; t < 8000000 && n < 132; t += 100) {
        m->clock += 100;
        if (!(io_read(m, 0xD20F) & 0x10)) low++;
        if (!(io_read(m, 0xD20E) & 0x20)) {
            if (!n) first = m->clock;
            got[n++] = io_read(m, 0xD20D);
            io_write(m, 0xD20E, 0x00);
            io_write(m, 0xD20E, 0x20);
            if (n == 10) {
                // The motor stops: nothing comes until it runs again.
                io_write(m, 0xD302, 0x3C);
                for (int i = 0; i < 100; i++) {
                    m->clock += 1000;
                    assert(io_read(m, 0xD20E) & 0x20);
                    assert(io_read(m, 0xD20F) & 0x10);
                }
                io_write(m, 0xD302, 0x34);
            }
        }
    }
    assert(n == 132 && memcmp(got, r4, 132) == 0);
    // The gap, then the first byte's ten bits; the line was low for its zeros.
    uint64_t byte = (uint64_t)(CPU_HZ * 10 / 600), gap = (uint64_t)(250 * (CPU_HZ / 1000));
    assert(first - on >= gap + byte && first - on < gap + byte + 200);
    assert(low > 0);
    io_write(m, 0xD302, 0x3C);

    // RESET boots the tape again from the start.
    machine_warm_reset(m);
    for (int i = 0; i < 3; i++) machine_run_frame(m);
    check_booted(m);

    // The next tape goes in without a reset, rewound.
    uint64_t clock = m->clock;
    assert(!machine_load_file(m, "side2.cas", cas.data, cas.size));
    assert(m->clock == clock && m->ram[0x3000] == 1 && m->tape.record == 0);

    // Not a tape, an empty one, and a turbo one.
    assert(machine_load_file(m, "bad.cas", cas.data + 1, cas.size - 1));
    Buf empty = {0}, turbo = {0};
    chunk(&empty, "FUJI", 0, NULL, 0);
    assert(machine_load_file(m, "empty.cas", empty.data, empty.size));
    chunk(&turbo, "FUJI", 0, NULL, 0);
    chunk(&turbo, "fsk ", 0, (const uint8_t *)"\1\0\2\0", 4);
    assert(machine_load_file(m, "turbo.cas", turbo.data, turbo.size));
    assert(m->tape.data);

    // A file read through CIO, after a leader: the padding record left on
    // the boot file goes by unread.
    if (!asm6502(cioboot, code, 0x0700, sizeof code, err, sizeof err)) {
        fprintf(stderr, "CIO boot code: %s\n", err);
        return 1;
    }
    Buf cio = {0};
    uint8_t rec[132], full[128], part[128];
    chunk(&cio, "FUJI", 0, NULL, 0);
    record(&cio, 2000, 0xFC, code, rec);
    record(&cio, 250, 0xFC, code + 128, rec);
    memset(data, 0xEE, sizeof data);
    record(&cio, 250, 0xFC, data, rec);
    for (int i = 0; i < 128; i++) full[i] = i;
    record(&cio, 10000, 0xFC, full, rec);
    memset(part, 0, sizeof part);
    for (int i = 0; i < 50; i++) part[i] = 0x80 + i;
    part[127] = 50;
    record(&cio, 250, 0xFA, part, rec);
    record(&cio, 250, 0xFE, data, rec);
    machine_eject_file(m);
    assert(!machine_load_file(m, "cio.cas", cio.data, cio.size));
    for (int i = 0; i < 3; i++) machine_run_frame(m);
    const uint8_t *r = m->ram + 0x3010;
    assert(r[0] == 0x82);                 // K: is not there
    assert(r[1] == 0x01);                 // C: opens
    assert(r[2] == 0x01 && r[3] == 100);  // 100 bytes
    assert(r[4] == 0x01 && r[5] == 100);  // the 101st, in A
    // 27 bytes left of the full record, the partial record's 50, then
    // the end of the file.
    assert(r[6] == 0x88 && r[7] == 77);
    assert(m->ram[0x5000] == 101 && m->ram[0x501A] == 127 && m->ram[0x501B] == 0x80 && m->ram[0x504C] == 0x80 + 49);
    assert(r[8] == 0x01);                 // CLOSE

    // Without a tape, nothing boots from it.
    machine_eject_file(m);
    machine_cold_reset(m);
    for (int i = 0; i < 3; i++) machine_run_frame(m);
    assert(m->ram[0x3000] == 0 && !m->tape.data);

    machine_free(m);
    free(cas.data);
    free(empty.data);
    free(turbo.data);
    free(cio.data);
    puts("CAS tapes: pass");
    return 0;
}
