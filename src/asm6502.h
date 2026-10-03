#ifndef SALLY_ASM6502_H
#define SALLY_ASM6502_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Assembles `src` into `mem`, which holds the addresses [base, base + size).
// On failure returns false with a message in `err`.
bool asm6502(const char *src, uint8_t *mem, int base, int size, char *err, size_t errlen);

#endif
