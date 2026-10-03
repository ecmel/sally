// A small two-pass 6502 assembler, enough to build the OS from source at
// startup.
//
//   label:  lda #<expr      ; comment
//   NAME = expr
//   .org expr / .byte list / .word list / .screen "text"
//
// Expressions are numbers ($hex, %binary, decimal, 'c'), symbols and `*`
// joined by + and -, with an optional leading < or > for the low or high
// byte. Zero page addressing is used when the operand's value is known on
// the first pass and below $100, so equates must come before their use.

#include "asm6502.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL, NMODES };

typedef struct {
    const char *name;
    int16_t op[NMODES];
} Ins;

#define _ -1
static const Ins table[] = {
    //         IMP   ACC   IMM   ZP    ZPX   ZPY   ABS   ABX   ABY   IND   IZX   IZY   REL
    {"ADC", {_, _, 0x69, 0x65, 0x75, _, 0x6D, 0x7D, 0x79, _, 0x61, 0x71, _}},
    {"AND", {_, _, 0x29, 0x25, 0x35, _, 0x2D, 0x3D, 0x39, _, 0x21, 0x31, _}},
    {"ASL", {_, 0x0A, _, 0x06, 0x16, _, 0x0E, 0x1E, _, _, _, _, _}},
    {"BCC", {_, _, _, _, _, _, _, _, _, _, _, _, 0x90}},
    {"BCS", {_, _, _, _, _, _, _, _, _, _, _, _, 0xB0}},
    {"BEQ", {_, _, _, _, _, _, _, _, _, _, _, _, 0xF0}},
    {"BIT", {_, _, _, 0x24, _, _, 0x2C, _, _, _, _, _, _}},
    {"BMI", {_, _, _, _, _, _, _, _, _, _, _, _, 0x30}},
    {"BNE", {_, _, _, _, _, _, _, _, _, _, _, _, 0xD0}},
    {"BPL", {_, _, _, _, _, _, _, _, _, _, _, _, 0x10}},
    {"BRK", {0x00, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"BVC", {_, _, _, _, _, _, _, _, _, _, _, _, 0x50}},
    {"BVS", {_, _, _, _, _, _, _, _, _, _, _, _, 0x70}},
    {"CLC", {0x18, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"CLD", {0xD8, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"CLI", {0x58, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"CLV", {0xB8, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"CMP", {_, _, 0xC9, 0xC5, 0xD5, _, 0xCD, 0xDD, 0xD9, _, 0xC1, 0xD1, _}},
    {"CPX", {_, _, 0xE0, 0xE4, _, _, 0xEC, _, _, _, _, _, _}},
    {"CPY", {_, _, 0xC0, 0xC4, _, _, 0xCC, _, _, _, _, _, _}},
    {"DEC", {_, _, _, 0xC6, 0xD6, _, 0xCE, 0xDE, _, _, _, _, _}},
    {"DEX", {0xCA, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"DEY", {0x88, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"EOR", {_, _, 0x49, 0x45, 0x55, _, 0x4D, 0x5D, 0x59, _, 0x41, 0x51, _}},
    {"INC", {_, _, _, 0xE6, 0xF6, _, 0xEE, 0xFE, _, _, _, _, _}},
    {"INX", {0xE8, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"INY", {0xC8, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"JMP", {_, _, _, _, _, _, 0x4C, _, _, 0x6C, _, _, _}},
    {"JSR", {_, _, _, _, _, _, 0x20, _, _, _, _, _, _}},
    {"LDA", {_, _, 0xA9, 0xA5, 0xB5, _, 0xAD, 0xBD, 0xB9, _, 0xA1, 0xB1, _}},
    {"LDX", {_, _, 0xA2, 0xA6, _, 0xB6, 0xAE, _, 0xBE, _, _, _, _}},
    {"LDY", {_, _, 0xA0, 0xA4, 0xB4, _, 0xAC, 0xBC, _, _, _, _, _}},
    {"LSR", {_, 0x4A, _, 0x46, 0x56, _, 0x4E, 0x5E, _, _, _, _, _}},
    {"NOP", {0xEA, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"ORA", {_, _, 0x09, 0x05, 0x15, _, 0x0D, 0x1D, 0x19, _, 0x01, 0x11, _}},
    {"PHA", {0x48, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"PHP", {0x08, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"PLA", {0x68, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"PLP", {0x28, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"ROL", {_, 0x2A, _, 0x26, 0x36, _, 0x2E, 0x3E, _, _, _, _, _}},
    {"ROR", {_, 0x6A, _, 0x66, 0x76, _, 0x6E, 0x7E, _, _, _, _, _}},
    {"RTI", {0x40, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"RTS", {0x60, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"SBC", {_, _, 0xE9, 0xE5, 0xF5, _, 0xED, 0xFD, 0xF9, _, 0xE1, 0xF1, _}},
    {"SEC", {0x38, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"SED", {0xF8, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"SEI", {0x78, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"STA", {_, _, _, 0x85, 0x95, _, 0x8D, 0x9D, 0x99, _, 0x81, 0x91, _}},
    {"STX", {_, _, _, 0x86, _, 0x96, 0x8E, _, _, _, _, _, _}},
    {"STY", {_, _, _, 0x84, 0x94, _, 0x8C, _, _, _, _, _, _}},
    {"TAX", {0xAA, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"TAY", {0xA8, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"TSX", {0xBA, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"TXA", {0x8A, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"TXS", {0x9A, _, _, _, _, _, _, _, _, _, _, _, _}},
    {"TYA", {0x98, _, _, _, _, _, _, _, _, _, _, _, _}},
};
#undef _

typedef struct {
    char name[32];
    int value;
    bool defined;
} Sym;

typedef struct {
    Sym syms[512];
    int nsyms;
    int pass;
    int pc;
    uint8_t *mem;
    int base;
    int size;
    int line;
    char *err;
    size_t errlen;
    bool failed;
} Asm;

static void fail(Asm *as, const char *fmt, ...) {
    if (as->failed) return;
    as->failed = true;
    int n = snprintf(as->err, as->errlen, "line %d: ", as->line);
    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < as->errlen) vsnprintf(as->err + n, as->errlen - n, fmt, ap);
    va_end(ap);
}

static Sym *find(Asm *as, const char *name, size_t len) {
    for (int i = 0; i < as->nsyms; i++)
        if (strlen(as->syms[i].name) == len && strncasecmp(as->syms[i].name, name, len) == 0) return &as->syms[i];
    return NULL;
}

static void define(Asm *as, const char *name, size_t len, int value) {
    Sym *s = find(as, name, len);
    if (!s) {
        if (as->nsyms == (int)(sizeof as->syms / sizeof as->syms[0]) || len >= sizeof s->name) {
            fail(as, "too many symbols");
            return;
        }
        s = &as->syms[as->nsyms++];
        memcpy(s->name, name, len);
        s->name[len] = 0;
    } else if (as->pass == 1 && s->defined) {
        fail(as, "%s defined twice", s->name);
        return;
    } else if (as->pass == 2 && s->value != value) {
        fail(as, "%s moved between passes", s->name);
        return;
    }
    s->value = value;
    s->defined = true;
}

static const char *skip(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Parses an expression at *pp. `known` is cleared when it uses a symbol
// not yet defined.
static int expr(Asm *as, const char **pp, bool *known) {
    const char *p = skip(*pp);
    int part = 0;
    if (*p == '<' || *p == '>') part = *p++;
    int value = 0, sign = 1;
    for (;;) {
        p = skip(p);
        int term = 0;
        if (*p == '$') {
            p++;
            if (!isxdigit((unsigned char)*p)) fail(as, "bad hex number");
            term = (int)strtol(p, (char **)&p, 16);
        } else if (*p == '%') {
            p++;
            term = (int)strtol(p, (char **)&p, 2);
        } else if (isdigit((unsigned char)*p)) {
            term = (int)strtol(p, (char **)&p, 10);
        } else if (*p == '\'' && p[1] && p[2] == '\'') {
            term = (unsigned char)p[1];
            p += 3;
        } else if (*p == '*') {
            term = as->pc;
            p++;
        } else if (ident_char(*p)) {
            const char *s = p;
            while (ident_char(*p)) p++;
            Sym *sym = find(as, s, p - s);
            if (sym && sym->defined) {
                term = sym->value;
            } else {
                *known = false;
                if (as->pass == 2) fail(as, "unknown symbol %.*s", (int)(p - s), s);
            }
        } else {
            fail(as, "expected an expression");
            break;
        }
        value += sign * term;
        p = skip(p);
        if (*p == '+')
            sign = 1;
        else if (*p == '-')
            sign = -1;
        else
            break;
        p++;
    }
    *pp = p;
    if (part == '<') return value & 0xFF;
    if (part == '>') return (value >> 8) & 0xFF;
    return value;
}

static void emit(Asm *as, int byte) {
    if (as->pass == 2) {
        int i = as->pc - as->base;
        if (i < 0 || i >= as->size)
            fail(as, "address $%04X is outside the image", as->pc);
        else
            as->mem[i] = (uint8_t)byte;
    }
    as->pc++;
}

// Ends with an optional ",X" or ",Y" at the end of `s` (length `len`).
static char index_reg(const char *s, size_t *len) {
    while (*len && (s[*len - 1] == ' ' || s[*len - 1] == '\t')) (*len)--;
    if (*len >= 2 && s[*len - 2] == ',') {
        char r = toupper((unsigned char)s[*len - 1]);
        if (r == 'X' || r == 'Y') {
            *len -= 2;
            return r;
        }
    }
    return 0;
}

static void instruction(Asm *as, const Ins *ins, const char *operand) {
    char buf[128];
    size_t len = strlen(operand);
    if (len >= sizeof buf) {
        fail(as, "operand too long");
        return;
    }
    memcpy(buf, operand, len + 1);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) buf[--len] = 0;

    int mode;
    bool known = true;
    int value = 0;
    const char *p = buf;
    if (len == 0) {
        mode = ins->op[IMP] >= 0 ? IMP : ACC;
    } else if (len == 1 && toupper((unsigned char)buf[0]) == 'A' && ins->op[ACC] >= 0) {
        mode = ACC;
    } else if (buf[0] == '#') {
        p++;
        value = expr(as, &p, &known);
        mode = IMM;
    } else if (buf[0] == '(') {
        p++;
        value = expr(as, &p, &known);
        p = skip(p);
        if (strncasecmp(p, ",X)", 3) == 0)
            mode = IZX;
        else if (strncasecmp(p, "),Y", 3) == 0)
            mode = IZY;
        else if (*p == ')')
            mode = IND;
        else {
            fail(as, "bad indirect operand");
            return;
        }
    } else {
        char r = index_reg(buf, &len);
        buf[len] = 0;
        value = expr(as, &p, &known);
        bool zp = known && value >= 0 && value < 0x100;
        if (ins->op[REL] >= 0)
            mode = REL;
        else if (r == 'X')
            mode = zp && ins->op[ZPX] >= 0 ? ZPX : ABX;
        else if (r == 'Y')
            mode = zp && ins->op[ZPY] >= 0 ? ZPY : ABY;
        else
            mode = zp && ins->op[ZP] >= 0 ? ZP : ABS;
    }
    if (as->failed) return;
    if (ins->op[mode] < 0) {
        fail(as, "%s does not take that addressing mode", ins->name);
        return;
    }
    emit(as, ins->op[mode]);
    switch (mode) {
    case IMP:
    case ACC: break;
    case IMM:
    case ZP:
    case ZPX:
    case ZPY:
    case IZX:
    case IZY: emit(as, value & 0xFF); break;
    case REL: {
        int off = value - (as->pc + 1);
        if (as->pass == 2 && (off < -128 || off > 127)) fail(as, "branch out of range");
        emit(as, off & 0xFF);
        break;
    }
    default:
        emit(as, value & 0xFF);
        emit(as, (value >> 8) & 0xFF);
        break;
    }
}

// ANTIC's internal character codes for ASCII text.
static int screen_code(int c) {
    if (c < 0x20) return c + 0x40;
    if (c < 0x60) return c - 0x20;
    return c;
}

static void directive(Asm *as, const char *name, size_t nlen, const char *p) {
    bool known = true;
    if (nlen == 3 && strncasecmp(name, "org", 3) == 0) {
        as->pc = expr(as, &p, &known);
        if (!known) fail(as, ".org needs a known address");
    } else if ((nlen == 4 && strncasecmp(name, "byte", 4) == 0) || (nlen == 4 && strncasecmp(name, "word", 4) == 0)) {
        bool word = tolower((unsigned char)name[0]) == 'w';
        for (;;) {
            int v = expr(as, &p, &known);
            emit(as, v & 0xFF);
            if (word) emit(as, (v >> 8) & 0xFF);
            p = skip(p);
            if (*p != ',') break;
            p++;
        }
    } else if (nlen == 6 && strncasecmp(name, "screen", 6) == 0) {
        p = skip(p);
        if (*p != '"') {
            fail(as, ".screen needs a string");
            return;
        }
        for (p++; *p && *p != '"'; p++) emit(as, screen_code((unsigned char)*p));
    } else {
        fail(as, "unknown directive .%.*s", (int)nlen, name);
    }
}

static void assemble_line(Asm *as, const char *line) {
    // Strip the comment, minding semicolons inside strings.
    char buf[256];
    size_t n = 0;
    bool quoted = false;
    for (const char *s = line; *s && *s != '\n' && n < sizeof buf - 1; s++) {
        if (*s == '"') quoted = !quoted;
        if (*s == ';' && !quoted) break;
        buf[n++] = *s;
    }
    buf[n] = 0;

    const char *p = skip(buf);
    while (*p) {
        const char *word = p;
        if (*p == '.') {
            p++;
            const char *name = p;
            while (ident_char(*p)) p++;
            directive(as, name, p - name, p);
            return;
        }
        while (ident_char(*p)) p++;
        size_t wlen = p - word;
        if (!wlen) {
            fail(as, "unexpected '%c'", *p);
            return;
        }
        const char *after = skip(p);
        if (*after == ':') {
            define(as, word, wlen, as->pc);
            p = skip(after + 1);
            continue;
        }
        if (*after == '=') {
            bool known = true;
            after++;
            int v = expr(as, &after, &known);
            if (!known) fail(as, "%.*s needs a known value", (int)wlen, word);
            define(as, word, wlen, v);
            return;
        }
        if (wlen == 3) {
            for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
                if (strncasecmp(table[i].name, word, 3) == 0) {
                    instruction(as, &table[i], after);
                    return;
                }
            }
        }
        fail(as, "unknown instruction %.*s", (int)wlen, word);
        return;
    }
}

bool asm6502(const char *src, uint8_t *mem, int base, int size, char *err, size_t errlen) {
    Asm *as = calloc(1, sizeof *as);
    if (!as) return false;
    as->mem = mem;
    as->base = base;
    as->size = size;
    as->err = err;
    as->errlen = errlen;
    for (as->pass = 1; as->pass <= 2 && !as->failed; as->pass++) {
        as->pc = base;
        as->line = 0;
        for (const char *line = src; *line && !as->failed;) {
            as->line++;
            assemble_line(as, line);
            const char *nl = strchr(line, '\n');
            if (!nl) break;
            line = nl + 1;
        }
    }
    bool ok = !as->failed;
    free(as);
    return ok;
}
