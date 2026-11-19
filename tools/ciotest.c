// The built-in OS's CIO: a handler added to HATABS, as DOS adds D:, and
// called through CIOV for each command.
#include "asm6502.h"
#include "machine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// T:, a test handler at $0600. OPEN keeps the unit and AUX1, CLOSE
// counts, GET reads "AB", EOL, "CD" and then the end of file, PUT writes
// from $3100 on, STATUS keeps the IOCB's handler entry and SPECIAL its
// command.
static const char *const handler =
    "ICHIDZ = $20\n"
    "ICDNOZ = $21\n"
    "ICCOMZ = $22\n"
    "ICAX1Z = $2A\n"
    "        .org $0600\n"
    "        .word open-1, close-1, get-1, put-1, status-1, special-1\n"
    "open:   lda ICDNOZ\n"
    "        sta $3010\n"
    "        lda ICAX1Z\n"
    "        sta $3011\n"
    "        ldy #1\n"
    "        rts\n"
    "close:  inc $3012\n"
    "        ldy #1\n"
    "        rts\n"
    "get:    ldx $3013\n"
    "        cpx #5\n"
    "        beq eof\n"
    "        inc $3013\n"
    "        lda text,x\n"
    "        ldy #1\n"
    "        rts\n"
    "eof:    ldy #$88\n"
    "        rts\n"
    "put:    ldx $3014\n"
    "        sta $3100,x\n"
    "        inc $3014\n"
    "        ldy #1\n"
    "        rts\n"
    "status: lda ICHIDZ\n"
    "        sta $3015\n"
    "        ldy #1\n"
    "        rts\n"
    "special: lda ICCOMZ\n"
    "        sta $3016\n"
    "        ldy #1\n"
    "        rts\n"
    "text:   .byte 'A', 'B', $9B, 'C', 'D'\n"
    "        .org $0700\n"
    "done:   jmp done\n";

enum { DONE = 0x0700, HATABS = 0x031A, IOCB = 0x0340, NAMES = 0x3300 };

// Calls CIOV with X and A as given, returning to DONE. Returns Y.
static uint8_t call(Machine *m, uint8_t x, uint8_t a) {
    Cpu *c = &m->cpu;
    m->ram[0x100 + c->s--] = (DONE - 1) >> 8;
    m->ram[0x100 + c->s--] = (DONE - 1) & 0xFF;
    c->pc = CIOV;
    c->x = x;
    c->a = a;
    for (int i = 0; i < 100000 && c->pc != DONE; i++) cpu_step(m);
    assert(c->pc == DONE);
    return c->y;
}

// Sets IOCB `n`'s command, buffer, length and (for OPEN) AUX1, then calls
// CIOV for it.
static uint8_t cio(Machine *m, int n, uint8_t cmd, uint16_t buf, uint16_t len, uint8_t ax1, uint8_t a) {
    uint8_t *b = m->ram + IOCB + n * 16;
    b[2] = cmd;
    b[4] = buf & 0xFF;
    b[5] = buf >> 8;
    b[8] = len & 0xFF;
    b[9] = len >> 8;
    if (cmd == 3) b[10] = ax1;
    return call(m, (uint8_t)(n * 16), a);
}

static uint16_t iocb_len(const Machine *m, int n) { return m->ram[IOCB + n * 16 + 8] | m->ram[IOCB + n * 16 + 9] << 8; }

int main(void) {
    Machine *m = machine_new();
    assert(m);
    for (int i = 0; i < 3; i++) machine_run_frame(m);
    // The IOCBs start closed.
    for (int n = 0; n < 8; n++) assert(m->ram[IOCB + n * 16] == 0xFF);

    char err[128];
    assert(asm6502(handler, m->ram + 0x0600, 0x0600, 0x0200, err, sizeof err));
    m->ram[HATABS] = 'T';
    m->ram[HATABS + 1] = 0x00;
    m->ram[HATABS + 2] = 0x06;
    strcpy((char *)m->ram + NAMES, "T2:");
    strcpy((char *)m->ram + NAMES + 8, "T:");
    strcpy((char *)m->ram + NAMES + 16, "Q:");

    // OPEN finds T: and passes on the unit and AUX1; ICPTL gets its PUT.
    assert(cio(m, 1, 3, NAMES, 0, 12, 0) == 0x01);
    assert(m->ram[0x3010] == 2 && m->ram[0x3011] == 12);
    assert(m->ram[IOCB + 16] == 0 && m->ram[IOCB + 17] == 2 && m->ram[IOCB + 19] == 0x01);
    assert(m->ram[IOCB + 22] == m->ram[0x0606] && m->ram[IOCB + 23] == m->ram[0x0607]);
    // An IOCB open already.
    assert(cio(m, 1, 3, NAMES, 0, 12, 0) == 0x81);

    // PUT CHARS, PUT RECORD (an EOL is added) and PUT with a length of 0.
    memcpy(m->ram + 0x3200, "XYZ", 3);
    assert(cio(m, 1, 11, 0x3200, 2, 0, 0) == 0x01 && iocb_len(m, 1) == 2);
    assert(cio(m, 1, 9, 0x3202, 1, 0, 0) == 0x01 && iocb_len(m, 1) == 1);
    assert(cio(m, 1, 11, 0x3200, 0, 0, 'W') == 0x01);
    assert(!memcmp(m->ram + 0x3100, "XYZ\x9BW", 5) && m->ram[0x3014] == 5);
    // The buffer address is left as it was.
    assert(m->ram[IOCB + 20] == 0x00 && m->ram[IOCB + 21] == 0x32);

    // GET RECORD stops after the EOL, GET CHARS with a length of 0 returns
    // the byte in A, and the end of file stops a GET CHARS short.
    assert(cio(m, 1, 5, 0x3400, 10, 0, 0) == 0x01 && iocb_len(m, 1) == 3);
    assert(!memcmp(m->ram + 0x3400, "AB\x9B", 3));
    assert(cio(m, 1, 7, 0x3400, 0, 0, 0) == 0x01 && m->cpu.a == 'C');
    assert(cio(m, 1, 7, 0x3400, 5, 0, 0) == 0x88 && iocb_len(m, 1) == 1 && m->ram[0x3400] == 'D');
    // A record longer than the buffer is read to its end and cut short.
    m->ram[0x3013] = 0;
    assert(cio(m, 1, 5, 0x3400, 1, 0, 0) == 0x89 && iocb_len(m, 1) == 1 && m->ram[0x3013] == 3);

    // STATUS and SPECIAL go to the handler.
    assert(cio(m, 1, 13, NAMES, 0, 0, 0) == 0x01 && m->ram[0x3015] == 0);
    assert(cio(m, 1, 40, NAMES, 0, 0, 0) == 0x01 && m->ram[0x3016] == 40);

    // CLOSE, then the IOCB is not open; a second CLOSE is fine.
    assert(cio(m, 1, 12, 0, 0, 0, 0) == 0x01 && m->ram[0x3012] == 1 && m->ram[IOCB + 16] == 0xFF);
    assert(cio(m, 1, 7, 0x3400, 1, 0, 0) == 0x85);
    assert(cio(m, 1, 12, 0, 0, 0, 0) == 0x01 && m->ram[0x3012] == 1);

    // SPECIAL on a closed IOCB finds the device for the call alone, as
    // DOS's delete and rename expect.
    assert(cio(m, 2, 33, NAMES + 8, 0, 0, 0) == 0x01 && m->ram[0x3016] == 33 && m->ram[IOCB + 32] == 0xFF);

    // A device not in HATABS, a bad IOCB, a bad command and a GET on an
    // IOCB opened for writing.
    assert(cio(m, 3, 3, NAMES + 16, 0, 4, 0) == 0x82 && m->ram[IOCB + 48] == 0xFF);
    assert(call(m, 0x15, 0) == 0x86);
    assert(cio(m, 0, 2, 0, 0, 0, 0) == 0x84);
    assert(cio(m, 4, 3, NAMES, 0, 8, 0) == 0x01);
    assert(cio(m, 4, 7, 0x3400, 1, 0, 0) == 0x83);

    machine_free(m);
    puts("CIO: pass");
    return 0;
}
