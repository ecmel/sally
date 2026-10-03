// Runs a cartridge headless, driven by a script, for checking the emulator
// without a window.
//
//   sallyrun [-os OSROM] [-l LABELS] ROM [SCRIPT]
//
// Script lines are `FRAME command [args]` (frames in decimal, other
// numbers in hex), run before their frame:
//
//   stick U/D/L/R/-   joystick from now on, e.g. `stick UL`
//   fire 0/1          trigger released or held
//   consol S/E/O/-    START, SELECT, OPTION held, e.g. `consol SO`
//   key HH            an Atari key (keyboard code) pressed for one frame
//   shot NAME         the last frame as NAME.png
//   dump FILE         all 64K of memory as the CPU sees it, to FILE
//   keydown HH        an Atari key held down until `keyup`
//   keyup             releases it
//   regs              prints the CPU registers and the beam position
//   peek ADDR [N]     prints N bytes (hex, default 16) from ADDR
//   poke ADDR V...    writes bytes to RAM
//   bp ADDR           stops the next frames at ADDR and prints the registers
//                     (once per hit; `bp -` clears it)
//   watch ADDR        prints each instruction that changes ADDR (`watch -`
//                     clears it)
//   pokeylog FILE     writes the sound registers at the end of each frame:
//                     frame AUDF1 AUDC1 ... AUDF4 AUDC4 AUDCTL (hex)
//   wav FILE          records the sound from now on to FILE (16-bit mono)
//   prof N            profiles the next N frames: CPU cycles by routine
//   end               stops
//
// Addresses may be label names when a label file (ld65 -Ln or VICE
// format: `al 00C000 .name`) is given with -l.
//
// Without a script it runs 600 frames and saves shot.png. Either way it
// prints how long the emulation took.

#include "../src/machine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = n;
    return buf;
}

typedef struct {
    char name[48];
    uint16_t addr;
} Label;

static Label *labels;
static int nlabels;

static int by_addr(const void *a, const void *b) {
    return ((const Label *)a)->addr - ((const Label *)b)->addr;
}

static void load_labels(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(1);
    }
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned a;
        char name[64];
        if (sscanf(line, "al %x .%63s", &a, name) != 2) continue;
        if (name[0] == '_' && name[1] == '_') continue;  // linker symbols
        if (name[0] == '@') continue;                    // ca65 cheap locals
        if (nlabels == cap) {
            cap = cap ? cap * 2 : 256;
            labels = realloc(labels, cap * sizeof *labels);
        }
        snprintf(labels[nlabels].name, sizeof labels[nlabels].name, "%s", name);
        labels[nlabels].addr = (uint16_t)a;
        nlabels++;
    }
    fclose(f);
    qsort(labels, nlabels, sizeof *labels, by_addr);
}

// A number in hex, or a label.
static int parse_addr(const char *s) {
    for (int i = 0; i < nlabels; i++)
        if (strcmp(labels[i].name, s) == 0) return labels[i].addr;
    char *end;
    long v = strtol(s, &end, 16);
    if (*end) {
        fprintf(stderr, "unknown address %s\n", s);
        return -1;
    }
    return (int)v;
}

// The nearest code label at or below `addr` (only labels in the cartridge
// or OS areas, so RAM variables do not stand in for code).
static const char *label_for(uint16_t addr, int *offset) {
    int lo = 0, hi = nlabels - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (labels[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0 || addr - labels[best].addr > 0x400) return NULL;
    *offset = addr - labels[best].addr;
    return labels[best].name;
}

static void print_regs(const Machine *m) {
    int off;
    const char *l = label_for(m->cpu.pc, &off);
    printf("PC=%04X", m->cpu.pc);
    if (l) printf(" (%s+%d)", l, off);
    printf(" A=%02X X=%02X Y=%02X S=%02X P=%c%c%c%c%c%c line=%d x=%d\n", m->cpu.a, m->cpu.x, m->cpu.y,
           m->cpu.s, m->cpu.n ? 'N' : '-', m->cpu.v ? 'V' : '-', m->cpu.d ? 'D' : '-', m->cpu.i ? 'I' : '-',
           m->cpu.z ? 'Z' : '-', m->cpu.c ? 'C' : '-', m->line, m->x);
}

static uint8_t peek(const Machine *m, uint16_t a) {
    const uint8_t *p = m->rmap[a >> 8];
    return p ? p[a & 0xFF] : 0xFF;
}

static void put16le(FILE *f, uint32_t v, int n) {
    for (int i = 0; i < n; i++) fputc((v >> (8 * i)) & 0xFF, f);
}

static uint32_t crc32(const uint8_t *p, size_t n, uint32_t c) {
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    put32(f, (uint32_t)n);
    uint8_t *body = malloc(n + 4);
    memcpy(body, type, 4);
    if (n) memcpy(body + 4, data, n);
    fwrite(body, 1, n + 4, f);
    put32(f, crc32(body, n + 4, 0));
    free(body);
}

// The frame as a PNG with each line doubled (uncompressed deflate).
static void save_png(const Machine *m, const char *path) {
    int w = FRAME_WIDTH, h = FRAME_HEIGHT * 2;
    size_t rawlen = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(rawlen), *r = raw;
    for (int y = 0; y < h; y++) {
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = gtia_palette[m->frame[(y / 2) * FRAME_WIDTH + x]];
            *r++ = c >> 16;
            *r++ = c >> 8;
            *r++ = c;
        }
    }
    size_t blocks = (rawlen + 65534) / 65535;
    size_t zlen = 2 + rawlen + blocks * 5 + 4;
    uint8_t *z = malloc(zlen), *zp = z;
    *zp++ = 0x78;
    *zp++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawlen; i += 65535) {
        size_t n = rawlen - i < 65535 ? rawlen - i : 65535;
        *zp++ = i + n == rawlen;
        *zp++ = n & 0xFF;
        *zp++ = n >> 8;
        *zp++ = ~n & 0xFF;
        *zp++ = (~n >> 8) & 0xFF;
        memcpy(zp, raw + i, n);
        zp += n;
    }
    for (size_t i = 0; i < rawlen; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = b << 16 | a;
    *zp++ = adler >> 24;
    *zp++ = adler >> 16;
    *zp++ = adler >> 8;
    *zp++ = adler;

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, zp - z);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

typedef struct {
    int frame;
    char cmd[16];
    char arg[256];
} Command;

static int by_frame(const void *a, const void *b) {
    return ((const Command *)a)->frame - ((const Command *)b)->frame;
}

int main(int argc, char **argv) {
    const char *os_path = NULL;
    int i = 1;
    while (i + 1 < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-os") == 0) {
            os_path = argv[i + 1];
        } else if (strcmp(argv[i], "-l") == 0) {
            load_labels(argv[i + 1]);
        } else {
            break;
        }
        i += 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: sallyrun [-os OSROM] [-l LABELS] ROM [SCRIPT]\n");
        return 2;
    }
    Machine *m = machine_new();
    if (os_path) {
        size_t n;
        uint8_t *os = read_file(os_path, &n);
        if (!os || !machine_set_os(m, os, n)) {
            fprintf(stderr, "%s: not a 16K OS ROM\n", os_path);
            return 1;
        }
        free(os);
    }
    size_t size;
    uint8_t *rom = read_file(argv[i], &size);
    if (!rom) {
        perror(argv[i]);
        return 1;
    }
    const char *err = machine_load_cart(m, rom, size);
    if (err) {
        fprintf(stderr, "%s: %s\n", argv[i], err);
        return 1;
    }
    machine_cold_reset(m);

    Command *cmds = NULL;
    int ncmds = 0, capcmds = 0;
    if (i + 1 < argc) {
        FILE *f = fopen(argv[i + 1], "r");
        if (!f) {
            perror(argv[i + 1]);
            return 1;
        }
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            Command c = {0};
            int n = sscanf(line, "%d %15s %255[^\n]", &c.frame, c.cmd, c.arg);
            if (n < 2) continue;
            if (ncmds == capcmds) {
                capcmds = capcmds ? capcmds * 2 : 256;
                cmds = realloc(cmds, capcmds * sizeof *cmds);
            }
            cmds[ncmds++] = c;
        }
        fclose(f);
        qsort(cmds, ncmds, sizeof cmds[0], by_frame);
    } else {
        cmds = calloc(2, sizeof *cmds);
        cmds[ncmds++] = (Command){600, "shot", "shot"};
        cmds[ncmds++] = (Command){600, "end", ""};
    }

    int key_frames = 0;
    int bp = -1, prof_frames = 0, watch = -1, watch_hits = 0;
    uint8_t watch_old = 0;
    uint64_t *prof = NULL;
    FILE *wav = NULL, *plog = NULL;
    long wav_samples = 0;
    double elapsed = 0, sum_sq = 0;
    long samples = 0;
    float peak = 0;
    int frames = 0;
    for (int next = 0;; frames++) {
        bool end = false;
        while (next < ncmds && cmds[next].frame <= frames) {
            Command *c = &cmds[next++];
            if (strcmp(c->cmd, "stick") == 0) {
                m->stick[0] = 0;
                for (char *s = c->arg; *s; s++)
                    m->stick[0] |= *s == 'U' ? 1 : *s == 'D' ? 2 : *s == 'L' ? 4 : *s == 'R' ? 8 : 0;
            } else if (strcmp(c->cmd, "fire") == 0) {
                m->trig[0] = c->arg[0] == '1';
            } else if (strcmp(c->cmd, "consol") == 0) {
                m->consol_keys = 0;
                for (char *s = c->arg; *s; s++)
                    m->consol_keys |= *s == 'S' ? 1 : *s == 'E' ? 2 : *s == 'O' ? 4 : 0;
            } else if (strcmp(c->cmd, "key") == 0) {
                machine_key(m, (uint8_t)strtol(c->arg, NULL, 16), true);
                key_frames = 2;
            } else if (strcmp(c->cmd, "shot") == 0) {
                char path[300];
                snprintf(path, sizeof path, "%s.png", c->arg);
                save_png(m, path);
            } else if (strcmp(c->cmd, "dump") == 0) {
                FILE *f = fopen(c->arg, "wb");
                for (int a = 0; a < 0x10000; a++) {
                    uint8_t *p = m->rmap[a >> 8];
                    fputc(p ? p[a & 0xFF] : 0xFF, f);
                }
                fclose(f);
            } else if (strcmp(c->cmd, "keydown") == 0) {
                machine_key(m, (uint8_t)strtol(c->arg, NULL, 16), true);
            } else if (strcmp(c->cmd, "keyup") == 0) {
                machine_key(m, 0, false);
            } else if (strcmp(c->cmd, "regs") == 0) {
                printf("frame %d: ", frames);
                print_regs(m);
            } else if (strcmp(c->cmd, "peek") == 0) {
                char a[64] = "";
                int n = 16;
                sscanf(c->arg, "%63s %x", a, &n);
                int addr = parse_addr(a);
                if (addr >= 0) {
                    printf("%04X:", addr);
                    for (int k = 0; k < n; k++) printf(" %02X", peek(m, (uint16_t)(addr + k)));
                    printf("\n");
                }
            } else if (strcmp(c->cmd, "poke") == 0) {
                char a[64] = "";
                int used = 0;
                sscanf(c->arg, "%63s%n", a, &used);
                int addr = parse_addr(a);
                const char *p = c->arg + used;
                unsigned v;
                int k;
                while (addr >= 0 && sscanf(p, "%x%n", &v, &k) == 1) {
                    m->ram[addr++ & 0xFFFF] = (uint8_t)v;
                    p += k;
                }
            } else if (strcmp(c->cmd, "bp") == 0) {
                bp = c->arg[0] == '-' ? -1 : parse_addr(c->arg);
            } else if (strcmp(c->cmd, "watch") == 0) {
                watch = c->arg[0] == '-' ? -1 : parse_addr(c->arg);
                if (watch >= 0) watch_old = peek(m, (uint16_t)watch);
            } else if (strcmp(c->cmd, "pokeylog") == 0) {
                plog = fopen(c->arg, "w");
                if (!plog) perror(c->arg);
            } else if (strcmp(c->cmd, "wav") == 0) {
                wav = fopen(c->arg, "wb");
                if (!wav) perror(c->arg);
                else fseek(wav, 44, SEEK_SET);
            } else if (strcmp(c->cmd, "prof") == 0) {
                prof_frames = atoi(c->arg);
                free(prof);
                prof = calloc(0x10000, sizeof *prof);
            } else if (strcmp(c->cmd, "end") == 0) {
                end = true;
            } else {
                fprintf(stderr, "unknown command %s\n", c->cmd);
            }
        }
        if (end || (next >= ncmds && ncmds > 0 && frames > cmds[ncmds - 1].frame)) break;
        if (key_frames && --key_frames == 0) machine_key(m, 0, false);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (bp >= 0 || prof_frames > 0 || watch >= 0) {
            // Instruction by instruction, for breakpoints and profiles.
            m->frame_done = false;
            bool hit = false;
            while (!m->frame_done) {
                uint16_t pc = m->cpu.pc;
                uint64_t c0 = m->clock;
                if (pc == bp && !hit) {
                    printf("frame %d: break at ", frames);
                    print_regs(m);
                    hit = true;
                }
                cpu_step(m);
                if (prof_frames > 0) prof[pc] += m->clock - c0;
                if (watch >= 0 && peek(m, (uint16_t)watch) != watch_old && watch_hits < 50) {
                    uint8_t v = peek(m, (uint16_t)watch);
                    int off;
                    const char *l = label_for(pc, &off);
                    printf("frame %d: %04X %02X -> %02X by %04X", frames, watch, watch_old, v, pc);
                    if (l) printf(" (%s+%d)", l, off);
                    printf(", now ");
                    print_regs(m);
                    watch_old = v;
                    watch_hits++;
                }
            }
            pokey_sync(m);
            if (prof_frames > 0 && --prof_frames == 0) {
                // Cycles by routine (nearest label), the busiest first.
                typedef struct { const char *name; uint64_t cycles; } Entry;
                Entry *e = calloc(nlabels + 1, sizeof *e);
                int ne = 0;
                uint64_t total = 0;
                for (int a = 0; a < 0x10000; a++) {
                    if (!prof[a]) continue;
                    total += prof[a];
                    int off;
                    const char *l = label_for((uint16_t)a, &off);
                    if (!l) l = "?";
                    int k;
                    for (k = 0; k < ne && e[k].name != l; k++) {}
                    if (k == ne) e[ne++].name = l;
                    e[k].cycles += prof[a];
                }
                for (int x = 0; x < ne; x++)
                    for (int y = x + 1; y < ne; y++)
                        if (e[y].cycles > e[x].cycles) {
                            Entry t = e[x];
                            e[x] = e[y];
                            e[y] = t;
                        }
                printf("profile: %llu cycles\n", (unsigned long long)total);
                for (int k = 0; k < ne && k < 25; k++)
                    printf("  %-24s %10llu %5.1f%%\n", e[k].name, (unsigned long long)e[k].cycles,
                           100.0 * e[k].cycles / total);
                free(e);
            }
        } else {
            machine_run_frame(m);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (plog) {
            fprintf(plog, "%d", frames);
            for (int ch = 0; ch < 4; ch++) fprintf(plog, " %02X %02X", m->pokey.audf[ch], m->pokey.audc[ch]);
            fprintf(plog, " %02X\n", m->pokey.audctl);
        }
        elapsed += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        for (int k = 0; k < m->pokey.naudio; k++) {
            float v = m->pokey.audio[k];
            if (wav) {
                float c = v * 2.0f;
                c = c > 1 ? 1 : c < -1 ? -1 : c;
                put16le(wav, (uint16_t)(int16_t)(c * 32767), 2);
                wav_samples++;
            }
            sum_sq += v * v;
            if (v > peak) peak = v;
            if (-v > peak) peak = -v;
        }
        samples += m->pokey.naudio;
        m->pokey.naudio = 0;
    }
    printf("%d frames, %.3f ms per frame, PC=%04X\n", frames, frames ? elapsed * 1000 / frames : 0, m->cpu.pc);
    printf("audio: %.1f samples per frame, peak %.3f, rms %.3f\n", frames ? (double)samples / frames : 0, peak,
           samples ? sqrt(sum_sq / samples) : 0);
    if (plog) fclose(plog);
    if (wav) {
        // The header, now that the length is known: 48 kHz, 16-bit mono.
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav);
        put16le(wav, 36 + wav_samples * 2, 4);
        fwrite("WAVEfmt ", 1, 8, wav);
        put16le(wav, 16, 4);
        put16le(wav, 1, 2);
        put16le(wav, 1, 2);
        put16le(wav, 48000, 4);
        put16le(wav, 96000, 4);
        put16le(wav, 2, 2);
        put16le(wav, 16, 2);
        fwrite("data", 1, 4, wav);
        put16le(wav, wav_samples * 2, 4);
        fclose(wav);
    }
    machine_free(m);
    free(rom);
    free(prof);
    free(cmds);
    free(labels);
    return 0;
}
