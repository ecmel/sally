// CPU tests: every opcode's cycle count against the NMOS 6502 table, then
// Klaus Dormann's functional test if its binary is given.
//
//   cputest [6502_functional_test.bin]

#include "../src/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cycles with no page crossing; branches not taken. 0 marks KIL.
static const uint8_t expected[256] = {
    7, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6, 2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6, 2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6, 2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6, 2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 6, 0, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 5, 0, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
};

// All of memory as plain RAM.
static void flat(Machine *m) {
    for (int p = 0; p < 256; p++) m->rmap[p] = m->wmap[p] = m->ram + p * 256;
}

static int check_cycles(Machine *m) {
    int bad = 0;
    for (int op = 0; op < 256; op++) {
        if (!expected[op]) continue;
        memset(m->ram, 0, sizeof m->ram);
        m->ram[0x0200] = op;
        m->ram[0x0201] = 0x10;
        m->ram[0x0202] = 0x00;
        m->ram[0x10] = 0x00;
        m->ram[0x11] = 0x03;
        Cpu *c = &m->cpu;
        memset(c, 0, sizeof *c);
        c->pc = 0x0200;
        c->s = 0xFF;
        m->nmi = m->irq = false;
        uint64_t before = m->cpu_cycles;
        cpu_step(m);
        int cycles = (int)(m->cpu_cycles - before);
        // With every flag clear, BPL, BVC, BCC and BNE branch.
        int want = expected[op] + (op == 0x10 || op == 0x50 || op == 0x90 || op == 0xD0);
        if (cycles != want) {
            printf("opcode %02X: %d cycles, expected %d\n", op, cycles, want);
            bad++;
        }
    }
    return bad;
}

static int functional(Machine *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    memset(m->ram, 0, sizeof m->ram);
    size_t n = fread(m->ram, 1, sizeof m->ram, f);
    fclose(f);
    if (n != 0x10000) {
        fprintf(stderr, "%s: expected a 64K image\n", path);
        return 1;
    }
    Cpu *c = &m->cpu;
    memset(c, 0, sizeof *c);
    c->pc = 0x0400;
    c->s = 0xFF;
    uint64_t start = m->cpu_cycles;
    for (;;) {
        uint16_t pc = c->pc;
        cpu_step(m);
        if (c->pc == pc) break;  // the test traps in a loop to itself
    }
    printf("functional test stopped at $%04X after %llu cycles: %s\n", c->pc,
           (unsigned long long)(m->cpu_cycles - start), c->pc == 0x3469 ? "pass" : "FAIL");
    return c->pc != 0x3469;
}

int main(int argc, char **argv) {
    Machine *m = machine_new();
    flat(m);
    int bad = check_cycles(m);
    printf("cycle counts: %s\n", bad ? "FAIL" : "pass");
    if (argc > 1) bad += functional(m, argv[1]);
    machine_free(m);
    return bad != 0;
}
