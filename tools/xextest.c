// XEX boot and segment loading through the same entry point as the apps.
#include "machine.h"

#include <assert.h>
#include <stdio.h>

static const uint8_t segmented[] = {
    0xFF, 0xFF,
    0x00, 0x20, 0x11, 0x20,  // $2000: first INIT
    0xA5, 0x14,              // LDA RTCLOK+2
    0xC5, 0x14,              // CMP RTCLOK+2
    0xF0, 0xFC,              // BEQ back to CMP (wait for vertical blank)
    0xAD, 0x00, 0x40,        // LDA $4000 before the next segment loads
    0x8D, 0x00, 0x30,        // STA $3000
    0xA9, 0xEF,              // LDA #$EF (select extended RAM for the CPU)
    0x8D, 0x01, 0xD3,        // STA PORTB
    0x60,                    // RTS
    0xE2, 0x02, 0xE3, 0x02, 0x00, 0x20,  // INITAD = $2000
    0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x40, 0x00, 0x40, 0x42,        // Goes into the selected bank
    0x00, 0x21, 0x06, 0x21,              // $2100: second INIT
    0xAD, 0x00, 0x40, 0x8D, 0x01, 0x30, 0x60,  // Copies $4000 to $3001, RTS
    0x00, 0x22, 0x06, 0x22,              // $2200: RUN
    0xAD, 0x01, 0x30, 0x8D, 0x02, 0x30, 0x60,  // Copies $3001 to $3002, RTS
    0xE0, 0x02, 0xE3, 0x02, 0x00, 0x22, 0x00, 0x21,  // RUNAD and final INITAD
};

static void check_segmented(Machine *m) {
    machine_run_frame(m);
    assert(m->xex_loading && m->ram[0x3002] == 0);
    machine_run_frame(m);
    assert(!m->xex_loading && m->cpu.s == 0xFF);
    assert(m->ram[0x3000] == 0 && m->ram[0x4000] == 0);
    assert(m->xram[0xC000] == 0x42 && m->ram[0x3002] == 0x42);
    assert(!m->cart && !m->rd5);
}

int main(void) {
    Machine *m = machine_new();
    assert(m);
    assert(!machine_load_file(m, "segmented.XEX", segmented, sizeof segmented));
    check_segmented(m);

    const uint8_t reversed[] = {0xFF, 0xFF, 0x01, 0x20, 0x00, 0x20, 0x60};
    assert(machine_load_file(m, "bad.xex", reversed, sizeof reversed));
    assert(machine_load_file(m, "truncated.xex", segmented, sizeof segmented - 1));
    assert(machine_load_file(m, "empty.xex", segmented, 2));
    assert(machine_load_file(m, "unsigned.xex", segmented + 2, sizeof segmented - 2));
    assert(m->ram[0x3002] == 0x42);

    machine_warm_reset(m);
    check_segmented(m);
    machine_cold_reset(m);
    check_segmented(m);

    const uint8_t implicit_run[] = {0xFF, 0xFF, 0x00, 0x20, 0x03, 0x20,
                                    0xEE, 0x02, 0x30, 0x60};  // INC $3002, RTS
    assert(!machine_load_file(m, "implicit.xex", implicit_run, sizeof implicit_run));
    machine_run_frame(m);
    assert(m->ram[0x3002] == 1);

    // A raw cartridge can also begin with $FFFF. Its extension decides.
    const uint8_t cart[0x2000] = {
        0xFF, 0xFF, 0xEE, 0x02, 0x30, 0x4C, 0x05, 0xA0, 0x60,
        [0x1FFA] = 0x02, [0x1FFB] = 0xA0, [0x1FFD] = 4,
        [0x1FFE] = 0x08, [0x1FFF] = 0xA0,
    };
    assert(!machine_load_file(m, "game.bin", cart, sizeof cart));
    machine_run_frame(m);
    assert(m->cart && m->rd5 && !m->xex && m->ram[0x3002] == 1);

    assert(!machine_load_file(m, "segmented.xex", segmented, sizeof segmented));
    check_segmented(m);
    machine_eject_file(m);
    machine_cold_reset(m);
    machine_run_frame(m);
    assert(!m->cart && !m->xex && m->ram[0x3002] == 0);
    machine_free(m);
    puts("XEX loading: pass");
    return 0;
}
