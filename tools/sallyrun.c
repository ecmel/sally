// Runs a cartridge headless, driven by a script, for checking the emulator
// without a window.
//
//   sallyrun [-os OSROM] ROM [SCRIPT]
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
//   end               stops
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
    if (i + 1 < argc && strcmp(argv[i], "-os") == 0) {
        os_path = argv[i + 1];
        i += 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: sallyrun [-os OSROM] ROM [SCRIPT]\n");
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

    Command cmds[1024];
    int ncmds = 0;
    if (i + 1 < argc) {
        FILE *f = fopen(argv[i + 1], "r");
        if (!f) {
            perror(argv[i + 1]);
            return 1;
        }
        char line[512];
        while (fgets(line, sizeof line, f) && ncmds < 1024) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            Command c = {0};
            int n = sscanf(line, "%d %15s %255s", &c.frame, c.cmd, c.arg);
            if (n >= 2) cmds[ncmds++] = c;
        }
        fclose(f);
        qsort(cmds, ncmds, sizeof cmds[0], by_frame);
    } else {
        cmds[ncmds++] = (Command){600, "shot", "shot"};
        cmds[ncmds++] = (Command){600, "end", ""};
    }

    int key_frames = 0;
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
        machine_run_frame(m);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        elapsed += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        for (int k = 0; k < m->pokey.naudio; k++) {
            float v = m->pokey.audio[k];
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
    machine_free(m);
    free(rom);
    return 0;
}
