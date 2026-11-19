// The machine as a whole: the XL memory map with 130XE banking, I/O
// dispatch, the PIA, the end of each scanline, resets, cartridges, XEX,
// disks and tapes.

#include "machine.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

Machine *machine_new(void) {
    static bool tables;
    if (!tables) {
        gtia_init();
        pokey_init();
        tables = true;
    }
    Machine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->pokey.cps = CPU_HZ / 48000.0;
    for (int i = 0; i < DRIVES; i++) m->disk[i].fd = -1;
    machine_set_os(m, NULL, 0);
    machine_cold_reset(m);
    return m;
}

void machine_free(Machine *m) {
    if (!m) return;
    free(m->cart);
    free(m->xex);
    for (int i = 0; i < DRIVES; i++) {
        free(m->disk[i].data);
        if (m->disk[i].fd >= 0) close(m->disk[i].fd);
    }
    free(m->tape.data);
    free(m->tape.records);
    free(m);
}

bool machine_set_os(Machine *m, const uint8_t *os, size_t size) {
    if (os && size == sizeof m->os) {
        memcpy(m->os, os, size);
        m->builtin_os = false;
        return true;
    }
    // No ROM, or a bad one: the built-in OS.
    m->builtin_os = true;
    char err[128];
    if (!os_build(m->os, err, sizeof err)) fprintf(stderr, "built-in OS: %s\n", err);
    return os == NULL;
}

void machine_set_basic(Machine *m, const uint8_t *basic, size_t size) {
    m->has_basic = basic && size == sizeof m->basic;
    if (m->has_basic) memcpy(m->basic, basic, size);
    machine_update_memory_map(m);
}

static void eject_xex(Machine *m) {
    free(m->xex);
    m->xex = NULL;
    m->xex_size = 0;
    m->xex_loading = false;
}

static void eject_disk(Disk *d) {
    free(d->data);
    free(d->atx);
    if (d->fd >= 0) close(d->fd);
    memset(d, 0, sizeof *d);
    d->fd = -1;
}

static void eject_disks(Machine *m) {
    for (int i = 0; i < DRIVES; i++) eject_disk(&m->disk[i]);
}

static void eject_tape(Machine *m) {
    free(m->tape.data);
    free(m->tape.records);
    memset(&m->tape, 0, sizeof m->tape);
    m->tape.start = m->tape.at = ~(uint64_t)0;
    m->cas.iocb = -1;
}

const char *machine_load_cart(Machine *m, const uint8_t *data, size_t size) {
    int type = CART_STANDARD;
    // A .car file is a 16-byte header ("CART", type, checksum) and the image.
    // Types 1 and 2 are standard 8K and 16K, 26-32 MegaCarts of 16K to 1M,
    // 54-56 SIC! cartridges of 128K to 512K.
    if (size >= 16 && memcmp(data, "CART", 4) == 0) {
        uint32_t car = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
        data += 16;
        size -= 16;
        size_t want = 0;
        if (car == 1) want = 0x2000;
        if (car == 2) want = 0x4000;
        if (car >= 26 && car <= 32) {
            type = CART_MEGACART;
            want = (size_t)0x4000 << (car - 26);
        }
        if (car >= 54 && car <= 56) {
            type = CART_SIC;
            want = (size_t)0x20000 << (car - 54);
        }
        if (!want) return "Only standard 8K and 16K, MegaCart and SIC! cartridges are supported so far.";
        if (size != want) return "The cartridge image is not the size its type says.";
    } else if (size != 0x2000 && size != 0x4000) {
        return "A cartridge image must be 8K or 16K, or a .car file.";
    }
    uint8_t *cart = malloc(size);
    if (!cart) return "Out of memory.";
    memcpy(cart, data, size);
    free(m->cart);
    eject_xex(m);
    eject_disks(m);
    eject_tape(m);
    m->cart = cart;
    m->cart_size = size;
    m->cart_type = type;
    m->cart_bank = 0;
    machine_update_memory_map(m);
    return NULL;
}

void machine_eject_cart(Machine *m) {
    free(m->cart);
    m->cart = NULL;
    m->cart_size = 0;
    machine_update_memory_map(m);
}

enum { DOSVEC = 0x000A, RUNAD = 0x02E0, INITAD = 0x02E2 };

static uint16_t word(const uint8_t *p) { return p[0] | p[1] << 8; }

// Each segment gives its first and last address, inclusive. A $FFFF
// marker is required at the start of the file and may repeat between segments.
static bool xex_segment(const uint8_t *data, size_t size, size_t *offset,
                        uint16_t *first, size_t *length) {
    size_t pos = *offset;
    while (size - pos >= 2 && word(data + pos) == 0xFFFF) pos += 2;
    if (size - pos < 4) return false;
    *first = word(data + pos);
    uint16_t last = word(data + pos + 2);
    pos += 4;
    if (last < *first) return false;
    *length = (size_t)last - *first + 1;
    if (*length > size - pos) return false;
    *offset = pos + *length;
    return true;
}

static const char *load_xex(Machine *m, const uint8_t *data, size_t size) {
    if (size < 2 || word(data) != 0xFFFF) return "An XEX file must start with $FFFF.";
    size_t offset = 0, length;
    uint16_t first;
    do {
        if (!xex_segment(data, size, &offset, &first, &length))
            return "The XEX file has an invalid or incomplete segment.";
    } while (offset < size);

    uint8_t *xex = malloc(size);
    if (!xex) return "Out of memory.";
    memcpy(xex, data, size);
    machine_eject_file(m);
    m->xex = xex;
    m->xex_size = size;
    return NULL;
}

// An ATR file is a 16-byte header and the sectors. The header starts with
// $0296 and gives the size of the sectors in 16-byte paragraphs (bytes 2-3,
// and 6 for the high part) and the sector size (bytes 4-5). APE sets bit
// 0 of byte 15 for a write protected disk.
static const char *parse_atr(const uint8_t *data, size_t size, Disk *d) {
    if (size < 16 || data[0] != 0x96 || data[1] != 0x02) return "The disk image is not an ATR file.";
    size_t len = ((size_t)data[2] | data[3] << 8 | data[6] << 16) * 16;
    int ss = data[4] | data[5] << 8;
    if (ss != 128 && ss != 256 && ss != 512) return "The disk image has sectors of an unsupported size.";
    // Some images are a little shorter or longer than the header says.
    if (len > size - 16) len = size - 16;
    memset(d, 0, sizeof *d);
    d->fd = -1;
    d->sector_size = ss;
    if (ss != 256 || len < 384) {
        d->sectors = (int)(len / (ss == 256 ? 128 : ss));
    } else if (len % 256 == 0) {
        d->padded = true;
        d->sectors = (int)(len / 256);
    } else {
        d->sectors = (int)(3 + (len - 384) / 256);
    }
    if (d->sectors > 0xFFFF) d->sectors = 0xFFFF;
    if (d->sectors == 0) return "The disk image has no sectors.";
    d->data = malloc(len);
    if (!d->data) return "Out of memory.";
    memcpy(d->data, data + 16, len);
    d->size = len;
    d->protect = data[15] & 1;
    return NULL;
}

// Opens the file for writes, unless the disk is write protected. A file
// that cannot be written, or no longer matches the image, protects it.
static void open_disk_file(Disk *d, const char *path, size_t size) {
    if (!path || d->protect) return;
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    struct stat st;
    if (d->fd >= 0 && fstat(d->fd, &st) == 0 && (size_t)st.st_size == size) return;
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
    d->protect = true;
}

static bool has_ext(const char *path, const char *want) {
    const char *ext = strrchr(path, '.');
    return ext && strcasecmp(ext, want) == 0;
}

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The disk turns at 288 rpm: a turn is 26042 units of 8 us.
enum { ATX_TURN = 26042 };

// Adds the sectors of each record of track `t` to the disk, from `n` on.
// A track record gives its size, type (0), the track number at 8, the
// sector count at 10 and the offset of its chunks at 20. Each chunk is
// its size, a type and a sector index (bytes 4 and 5) and a word of its
// own (6); a size of 0 ends them. The sector list (type 1) gives each
// sector's number, status, position and the offset of its data from the
// record, 8 bytes a sector; a weak sector chunk ($10) gives where in the
// sector its weak bits start.
static int atx_track(const uint8_t *data, size_t size, int t, AtxSector *out, int n) {
    for (size_t rec = le32(data + 28); rec + 32 <= size;) {
        size_t rsize = le32(data + rec);
        if (rsize < 32 || rsize > size - rec) break;
        const uint8_t *r = data + rec;
        int count = le16(r + 10), first = n;
        if (le16(r + 4) == 0 && r[8] == t) {
            for (int pass = 0; pass < 2; pass++) {
                for (size_t c = le32(r + 20); c + 8 <= rsize;) {
                    size_t csize = le32(r + c);
                    if (csize < 8 || csize > rsize - c) break;
                    int type = r[c + 4], index = r[c + 5];
                    if (pass == 0 && type == 1) {
                        for (int i = 0; i < count && 8 + 8 * (size_t)(i + 1) <= csize; i++) {
                            const uint8_t *e = r + c + 8 + 8 * i;
                            uint32_t at = le32(e + 4);
                            if (out) {
                                out[n] = (AtxSector){e[0], e[1], (uint16_t)(le16(e + 2) % ATX_TURN), 0, -1};
                                if (at) out[n].data = (uint32_t)(rec + at);
                            }
                            n++;
                        }
                    }
                    if (pass == 1 && type == 0x10 && out && index < n - first) out[first + index].weak = le16(r + c + 6);
                    c += csize;
                }
            }
        }
        rec += rsize;
    }
    return n;
}

// An ATX file is a 48-byte header, "AT8X", with the density at byte 18
// (0 single, 1 enhanced, 2 double) and the offset of the first track
// record at 28, then the track records. Each sector is kept as it was
// recorded, errors and duplicates included; the disk is write protected.
static const char *parse_atx(const uint8_t *data, size_t size, Disk *d) {
    memset(d, 0, sizeof *d);
    d->fd = -1;
    if (size < 48 || memcmp(data, "AT8X", 4) != 0) return "The disk image is not an ATX file.";
    if (data[18] > 2) return "The ATX image has a density that is not supported.";
    d->spt = data[18] == 1 ? 26 : 18;
    d->sector_size = data[18] == 2 ? 256 : 128;
    d->sectors = ATX_TRACKS * d->spt;
    int total = 0;
    for (int t = 0; t < ATX_TRACKS; t++) total = atx_track(data, size, t, NULL, total);
    if (!total) return "The disk image has no sectors.";
    d->atx = calloc(total, sizeof *d->atx);
    d->data = malloc(size);
    if (!d->atx || !d->data) {
        free(d->atx);
        free(d->data);
        d->atx = NULL;
        d->data = NULL;
        return "Out of memory.";
    }
    memcpy(d->data, data, size);
    d->size = size;
    int n = 0;
    for (int t = 0; t < ATX_TRACKS; t++) {
        d->track[t] = n;
        n = atx_track(data, size, t, d->atx, n);
    }
    d->track[ATX_TRACKS] = n;
    for (int i = 0; i < n; i++) {
        AtxSector *e = &d->atx[i];
        if (e->data && (e->data < 48 || e->data > size || size - e->data < (size_t)d->sector_size)) e->data = 0;
    }
    d->protect = true;
    return NULL;
}

static bool is_atr(const char *path) { return has_ext(path, ".atr"); }
static bool is_disk(const char *path) { return is_atr(path) || has_ext(path, ".atx"); }

static const char *parse_disk(const char *path, const uint8_t *data, size_t size, Disk *d) {
    if (!is_disk(path)) return "Only disk images can be opened together.";
    return is_atr(path) ? parse_atr(data, size, d) : parse_atx(data, size, d);
}

// A CAS file is a series of chunks: four letters of type, the length of
// the data and an auxiliary word (both little endian), then the data.
// "FUJI" starts it, "baud" sets the rate of the records after it (aux),
// and each "data" chunk is a record, with the gap before it in
// milliseconds (aux). The turbo chunks, "fsk " and "pwm?", are not
// supported.
static const char *parse_cas(const uint8_t *data, size_t size, Tape *t) {
    memset(t, 0, sizeof *t);
    t->start = t->at = ~(uint64_t)0;
    if (size < 8 || memcmp(data, "FUJI", 4) != 0) return "The tape image is not a CAS file.";
    int n = 0;
    for (size_t pos = 0; size - pos >= 8;) {
        const uint8_t *c = data + pos;
        size_t len = c[4] | c[5] << 8;
        if (len > size - pos - 8) return "The tape image is incomplete.";
        if (memcmp(c, "fsk ", 4) == 0 || memcmp(c, "pwm", 3) == 0)
            return "The tape is recorded in a turbo format, which is not supported.";
        if (memcmp(c, "data", 4) == 0 && len) n++;
        pos += 8 + len;
    }
    if (!n) return "The tape image holds no records.";
    t->records = calloc(n, sizeof *t->records);
    t->data = malloc(size);
    if (!t->records || !t->data) {
        free(t->records);
        free(t->data);
        t->records = NULL;
        t->data = NULL;
        return "Out of memory.";
    }
    memcpy(t->data, data, size);
    t->size = size;
    int baud = 600;
    for (size_t pos = 0; size - pos >= 8;) {
        const uint8_t *c = data + pos;
        int len = c[4] | c[5] << 8, aux = c[6] | c[7] << 8;
        if (memcmp(c, "baud", 4) == 0) baud = aux ? aux : 600;
        if (memcmp(c, "data", 4) == 0 && len)
            t->records[t->nrecords++] = (TapeRecord){pos + 8, len, aux, baud};
        pos += 8 + (size_t)len;
    }
    return NULL;
}

static void tape_cue(Machine *m, uint64_t from);

const char *machine_load_disks(Machine *m, int n, const char *const paths[], const uint8_t *const data[],
                               const size_t sizes[]) {
    if (n < 1) return "No disk images.";
    if (n > DRIVES) return "Sally has four disk drives.";
    Disk disks[DRIVES];
    for (int i = 0; i < n; i++) {
        const char *err = parse_disk(paths[i], data[i], sizes[i], &disks[i]);
        if (err) {
            while (i--) eject_disk(&disks[i]);
            return err;
        }
    }
    machine_eject_file(m);
    for (int i = 0; i < n; i++) {
        open_disk_file(&disks[i], paths[i], sizes[i]);
        m->disk[i] = disks[i];
    }
    machine_cold_reset(m);
    return NULL;
}

const char *machine_load_file(Machine *m, const char *path, const uint8_t *data, size_t size) {
    if (has_ext(path, ".cas")) {
        Tape t;
        const char *err = parse_cas(data, size, &t);
        if (err) return err;
        if (m->tape.data) {
            // The next tape: in the recorder, rewound, while the program runs.
            bool motor = m->tape.motor;
            eject_tape(m);
            m->tape = t;
            m->tape.motor = motor;
            tape_cue(m, m->clock);
            return NULL;
        }
        machine_eject_file(m);
        m->tape = t;
        machine_cold_reset(m);
        return NULL;
    }
    if (is_disk(path)) {
        if (!m->disk[0].data) return machine_load_disks(m, 1, &path, &data, &size);
        // The next disk of a set: swapped in while the program runs.
        Disk d;
        const char *err = parse_disk(path, data, size, &d);
        if (err) return err;
        eject_disk(&m->disk[0]);
        open_disk_file(&d, path, size);
        m->disk[0] = d;
        return NULL;
    }
    const char *ext = strrchr(path, '.');
    const char *err = ext && strcasecmp(ext, ".xex") == 0
        ? load_xex(m, data, size)
        : machine_load_cart(m, data, size);
    if (!err) machine_cold_reset(m);
    return err;
}

void machine_eject_file(Machine *m) {
    eject_xex(m);
    eject_disks(m);
    eject_tape(m);
    machine_eject_cart(m);
}

static void call_xex(Machine *m, uint16_t addr) {
    Cpu *c = &m->cpu;
    uint16_t ret = m->xex_resume_pc - 1;
    m->ram[0x100 | c->s--] = ret >> 8;
    m->ram[0x100 | c->s--] = ret & 0xFF;
    c->pc = addr;
}

bool machine_step_xex(Machine *m) {
    Cpu *c = &m->cpu;
    bool starting = m->xex_offset == 0;
    if (starting) {
        // The OS hands off to DOSVEC after setting up RAM and interrupts.
        if (c->pc == 0 || c->pc != word(m->ram + DOSVEC)) return false;
        m->xex_resume_pc = c->pc;
        m->xex_resume_s = c->s;
        m->ram[INITAD] = 0;
    } else if (c->pc != m->xex_resume_pc || c->s != m->xex_resume_s) {
        return false;
    }

    uint16_t first;
    size_t length;
    while (xex_segment(m->xex, m->xex_size, &m->xex_offset, &first, &length)) {
        if (starting) {
            // DOS defaults RUNAD to the load address of the first segment.
            m->ram[RUNAD] = first & 0xFF;
            m->ram[RUNAD + 1] = first >> 8;
            starting = false;
        }
        m->ram[INITAD + 1] = 0;
        const uint8_t *data = m->xex + m->xex_offset - length;
        for (size_t i = 0; i < length; i++) {
            uint16_t addr = first + i;
            uint8_t *page = m->wmap[addr >> 8];
            if (page) page[addr & 0xFF] = data[i];
            else io_write(m, addr, data[i]);
        }
        if (first <= INITAD + 1 && first + length > INITAD + 1) {
            uint16_t init = word(m->ram + INITAD);
            if (init) {
                // Resume loading once the INIT routine returns with RTS.
                call_xex(m, init);
                return true;
            }
        }
    }
    m->xex_loading = false;
    call_xex(m, word(m->ram + RUNAD));
    return true;
}

// The device control block that SIOV reads, at $0300.
enum {
    DDEVIC = 0x0300, DUNIT, DCOMND, DSTATS, DBUFLO, DBUFHI, DTIMLO, DUNUSE,
    DBYTLO, DBYTHI, DAUX1, DAUX2,
};

// SIO status codes.
enum { SIO_OK = 0x01, SIO_TIMEOUT = 0x8A, SIO_NAK = 0x8B, SIO_CHECKSUM = 0x8F, SIO_DEVICE_ERROR = 0x90 };

// Sector `n` (1-based) of the disk: its offset in the image and length.
static size_t disk_sector(const Disk *d, int n, int *len) {
    if (d->sector_size == 256 && n <= 3) {
        *len = 128;
        return (size_t)(n - 1) * (d->padded ? 256 : 128);
    }
    *len = d->sector_size;
    if (d->sector_size == 256 && !d->padded) return 384 + (size_t)(n - 4) * 256;
    return (size_t)(n - 1) * d->sector_size;
}

static uint8_t sio_peek(Machine *m, uint16_t addr) {
    const uint8_t *page = m->rmap[addr >> 8];
    return page ? page[addr & 0xFF] : 0xFF;
}

static void sio_poke(Machine *m, uint16_t addr, uint8_t v) {
    uint8_t *page = m->wmap[addr >> 8];
    if (page) page[addr & 0xFF] = v;
}

// Sends `len` bytes of `data` to the caller's buffer, as many as it asked for.
static void sio_send(Machine *m, const uint8_t *data, int len) {
    uint16_t buf = word(m->ram + DBUFLO);
    int want = word(m->ram + DBYTLO);
    for (int i = 0; i < len && i < want; i++) sio_poke(m, (uint16_t)(buf + i), data[i]);
}

// The PERCOM block of a 1050-style drive: the disk's geometry.
static void percom(const Disk *d, uint8_t out[12]) {
    int tracks = 1, spt = d->sectors;
    if (d->sectors % 40 == 0 && d->sectors / 40 <= 26) {
        tracks = 40;
        spt = d->sectors / 40;
    }
    uint8_t block[12] = {
        (uint8_t)tracks, 1, (uint8_t)(spt >> 8), (uint8_t)spt, 0,
        d->sector_size == 128 && d->sectors != 1040 ? 0 : 4,
        (uint8_t)(d->sector_size >> 8), (uint8_t)d->sector_size, 0xFF,
    };
    memcpy(out, block, sizeof block);
}

// The bytes of data a command sends to the drive: a sector for a write,
// the PERCOM block for 'O', none for the others.
static int disk_data_in(const Disk *d, uint8_t cmd, int n) {
    int len;
    switch (cmd) {
    case 'W':
    case 'P':
        if (n < 1 || n > d->sectors) return 0;
        disk_sector(d, n, &len);
        return len;
    case 'O': return 12;
    default: return 0;
    }
}

// Runs command `cmd` on sector or argument `n`. `buf` holds the data sent
// with it (see `disk_data_in`) and receives the data sent back, `*len`
// bytes. Returns the SIO status.
static uint8_t disk_command(Disk *d, uint8_t cmd, int n, uint8_t buf[512], int *len) {
    *len = 0;
    switch (cmd) {
    case 'R':
    case 'W':
    case 'P': {
        if (n < 1 || n > d->sectors) return SIO_NAK;
        int slen;
        size_t at = disk_sector(d, n, &slen);
        // A short image ends in a partial sector, read as zeros past the end.
        int have = at + slen <= d->size ? slen : at < d->size ? (int)(d->size - at) : 0;
        if (cmd == 'R') {
            memset(buf, 0, slen);
            memcpy(buf, d->data + at, have);
            *len = slen;
            return SIO_OK;
        }
        if (d->protect) return SIO_DEVICE_ERROR;
        memcpy(d->data + at, buf, have);
        if (d->fd >= 0 && pwrite(d->fd, d->data + at, have, 16 + (off_t)at) != have) return SIO_DEVICE_ERROR;
        return SIO_OK;
    }
    case 'S': {
        // Drive status (bit 3 write protected, 4 motor on, 5 double
        // density, 7 enhanced density), the controller's inverted status
        // (bit 6 low when protected; for ATX, as the last command left
        // it), the format timeout.
        uint8_t status[4] = {
            (uint8_t)(0x10 | (d->protect ? 0x08 : 0) | (d->sector_size == 256 ? 0x20 : 0) |
                      (d->sector_size == 128 && d->sectors == 1040 ? 0x80 : 0)),
            d->atx ? (uint8_t)~d->fdc : d->protect ? 0xBF : 0xFF, 0xE0, 0,
        };
        memcpy(buf, status, sizeof status);
        *len = sizeof status;
        return SIO_OK;
    }
    case '!':
    case '"': {
        // Format: the disk keeps its geometry; no sector is bad.
        if (d->protect) return SIO_DEVICE_ERROR;
        memset(d->data, 0, d->size);
        if (d->fd >= 0 && pwrite(d->fd, d->data, d->size, 16) != (ssize_t)d->size) return SIO_DEVICE_ERROR;
        memset(buf, 0xFF, d->sector_size);
        *len = d->sector_size;
        return SIO_OK;
    }
    case 'N':
        percom(d, buf);
        *len = 12;
        return SIO_OK;
    case 'O': return SIO_OK;
    default: return SIO_NAK;
    }
}

static void mark_used(Machine *m, int drive) { m->disk_used[drive] = m->clock | 1; }

unsigned machine_disk_activity(const Machine *m) {
    unsigned bits = 0;
    for (int i = 0; i <= DRIVES; i++)
        if (m->disk_used[i] && m->clock - m->disk_used[i] < (uint64_t)(CPU_HZ / 8)) bits |= 1u << i;
    return bits;
}

static uint8_t sio_checksum(const uint8_t *p, int n);

// POKEY takes whichever byte comes in first, a drive's or the tape's.
static void serin_schedule(Machine *m) {
    uint64_t a = m->sio.at, b = m->tape.at;
    m->pokey.serin_at = a < b ? a : b;
}

// A byte on tape is ten bits at its record's rate.
static uint64_t tape_byte(const TapeRecord *r) { return (uint64_t)(CPU_HZ * 10 / r->baud); }

// Times the tape's next byte from cycle `from`, while the motor runs: a
// record starts after its gap.
static void tape_cue(Machine *m, uint64_t from) {
    Tape *t = &m->tape;
    t->start = t->at = ~(uint64_t)0;
    if (t->data && t->motor && t->record < t->nrecords) {
        const TapeRecord *r = &t->records[t->record];
        t->start = from + (t->pos ? 0 : (uint64_t)(r->gap_ms * (CPU_HZ / 1000)));
        t->at = t->start + tape_byte(r);
    }
    serin_schedule(m);
}

// The motor, PIA CA2: on when PACTL's bits 3-5 are 110. A byte cut off
// when it stops plays again from its start.
static void tape_motor(Machine *m, bool on) {
    Tape *t = &m->tape;
    if (on == t->motor) return;
    t->motor = on;
    tape_cue(m, m->clock);
}

static uint8_t tape_next(Machine *m) {
    Tape *t = &m->tape;
    const TapeRecord *r = &t->records[t->record];
    uint8_t v = t->data[r->offset + t->pos];
    if (++t->pos == r->length) {
        t->record++;
        t->pos = 0;
    }
    mark_used(m, DRIVES);
    tape_cue(m, t->at);
    return v;
}

bool machine_serial_line(const Machine *m) {
    const Tape *t = &m->tape;
    if (t->at == ~(uint64_t)0 || m->clock < t->start || m->clock >= t->at) return true;
    int bit = (int)((m->clock - t->start) * 10 / (t->at - t->start));
    if (bit == 0) return false;  // start bit
    if (bit == 9) return true;   // stop bit
    const TapeRecord *r = &t->records[t->record];
    return (t->data[r->offset + t->pos] >> (bit - 1)) & 1;
}

// A cassette read: the next whole record, its first DBYT bytes and the
// checksum after them. A record already begun is lost, as it would be.
// Tapes are not written.
static uint8_t tape_read(Machine *m, uint8_t *out, int want) {
    Tape *t = &m->tape;
    m->hold_start = 0;
    if (!t->data) return SIO_TIMEOUT;
    if (t->pos) {
        t->record++;
        t->pos = 0;
    }
    if (t->record >= t->nrecords) return SIO_TIMEOUT;
    mark_used(m, DRIVES);
    const TapeRecord *r = &t->records[t->record++];
    tape_cue(m, m->clock);
    const uint8_t *p = t->data + r->offset;
    if (r->length < want + 1) return SIO_TIMEOUT;
    memcpy(out, p, want);
    return sio_checksum(p, want) == p[want] ? SIO_OK : SIO_CHECKSUM;
}

static uint8_t tape_sio(Machine *m, uint8_t *chksum) {
    if (m->ram[DCOMND] != 'R') {
        m->hold_start = 0;
        return SIO_TIMEOUT;
    }
    uint8_t buf[1024];
    int want = word(m->ram + DBYTLO);
    if (want > (int)sizeof buf) want = sizeof buf;
    uint8_t status = tape_read(m, buf, want);
    uint16_t to = word(m->ram + DBUFLO);
    if (status == SIO_OK || status == SIO_CHECKSUM) {
        for (int i = 0; i < want; i++) sio_poke(m, (uint16_t)(to + i), buf[i]);
        *chksum = sio_checksum(buf, want);
    }
    return status;
}

// Returns from a routine the machine answered, with `status` in Y and N
// set on an error.
static void trap_return(Machine *m, uint8_t status) {
    Cpu *c = &m->cpu;
    c->y = status;
    c->n = status & 0x80;
    c->z = false;
    // RTS
    uint16_t ret = m->ram[0x100 | (uint8_t)(c->s + 1)] | m->ram[0x100 | (uint8_t)(c->s + 2)] << 8;
    c->s += 2;
    c->pc = ret + 1;
}

// The routine at `addr` is the OS ROM's: not RAM under it, nor code
// jumped to there.
static bool in_os(const Machine *m, uint16_t addr) {
    return m->rmap[addr >> 8] == m->os + ((addr - 0xC000) & ~0xFF);
}

// CIO's IOCBs, 16 bytes each from $0340, and its status codes.
enum {
    IOCB = 0x0340, ICHID = 0, ICDNO, ICCOM, ICSTA, ICBAL, ICBAH, ICPTL, ICPTH, ICBLL, ICBLH, ICAX1, ICAX2,
};
enum {
    CIO_OK = 0x01, CIO_TRUNCATED = 0x89, CIO_EOF = 0x88, CIO_BAD_IOCB = 0x86, CIO_NOT_OPEN = 0x85,
    CIO_BAD_COMMAND = 0x84, CIO_OPEN = 0x81, CIO_NO_DEVICE = 0x82, CIO_READ_ONLY = 0x87,
};

// The next byte of the file on C:. Records hold 128 bytes ($FC), fewer
// with their count in the last byte ($FA), or mark the end ($FE).
static uint8_t cas_get(Machine *m, uint8_t *v) {
    CasFile *f = &m->cas;
    while (f->pos >= f->len) {
        if (f->eof) return CIO_EOF;
        uint8_t rec[131];
        uint8_t status = tape_read(m, rec, sizeof rec);
        if (status != SIO_OK) return status;
        memcpy(f->data, rec + 3, 128);
        f->pos = 0;
        f->len = 128;
        if (rec[2] == 0xFA) f->len = rec[130] & 0x7F;
        if (rec[2] == 0xFE) {
            f->eof = true;
            f->len = 0;
        }
    }
    *v = f->data[f->pos++];
    return CIO_OK;
}

// A file on tape starts after a leader, seconds of steady tone. On a real
// recorder the tape runs on past the end of the last file while the OS
// waits for a key press to open the next, so whatever is left of it goes
// by unread.
enum { TAPE_LEADER_MS = 5000 };

static void tape_find_leader(Tape *t) {
    if (t->pos) {
        t->record++;
        t->pos = 0;
    }
    while (t->record > 0 && t->record < t->nrecords && t->records[t->record].gap_ms < TAPE_LEADER_MS) t->record++;
}

// CIOV takes the request from the IOCB that X points at and returns with
// the status in Y and the IOCB, N set on an error, and a byte read in A.
// C: opens for reading only, at the start of the next file on the tape
// and without the beep and key press of the Atari OS.
bool machine_cio(Machine *m) {
    if (!m->builtin_os || !in_os(m, CIOV)) return false;
    Cpu *c = &m->cpu;
    uint8_t status = CIO_OK;
    CasFile *f = &m->cas;
    uint16_t io = IOCB + c->x;
    uint8_t *b = m->ram + io;
    int iocb = c->x >> 4;
    if ((c->x & 0x0F) || c->x >= 0x80) {
        status = CIO_BAD_IOCB;
    } else if (b[ICCOM] == 3) {
        // OPEN: the device is the first letter of the name.
        uint8_t dev = sio_peek(m, word(b + ICBAL));
        if (f->iocb == iocb) status = CIO_OPEN;
        else if (dev != 'C' || !m->tape.data) status = CIO_NO_DEVICE;
        else if (b[ICAX1] & 0x08) status = CIO_READ_ONLY;
        else if (f->iocb >= 0) status = CIO_OPEN;
        else {
            tape_find_leader(&m->tape);
            tape_cue(m, m->clock);
            f->iocb = iocb;
            f->eof = false;
            f->len = f->pos = 0;
            b[ICHID] = 0;
            b[ICDNO] = 1;
        }
    } else if (f->iocb != iocb) {
        status = b[ICCOM] == 12 ? CIO_OK : CIO_NOT_OPEN;
    } else if (b[ICCOM] == 12) {
        // CLOSE
        f->iocb = -1;
        b[ICHID] = 0xFF;
    } else if (b[ICCOM] == 13) {
        // STATUS
    } else if (b[ICCOM] == 5 || b[ICCOM] == 7) {
        // GET RECORD stops after an end of line ($9B); GET BYTES with a
        // length of 0 reads one byte into A.
        uint16_t to = word(b + ICBAL);
        int len = word(b + ICBLL), n = 0;
        uint8_t v = 0;
        if (len == 0 && b[ICCOM] == 7) {
            status = cas_get(m, &v);
            if (status == CIO_OK) c->a = v;
        } else {
            while (n < len) {
                status = cas_get(m, &v);
                if (status != CIO_OK) break;
                sio_poke(m, (uint16_t)(to + n++), v);
                if (b[ICCOM] == 5 && v == 0x9B) break;
            }
            if (status == CIO_OK && b[ICCOM] == 5 && v != 0x9B) status = CIO_TRUNCATED;
            if (n) c->a = v;
            b[ICBLL] = n & 0xFF;
            b[ICBLH] = n >> 8;
        }
    } else {
        status = CIO_BAD_COMMAND;
    }
    if (!((c->x & 0x0F) || c->x >= 0x80)) b[ICSTA] = status;
    trap_return(m, status);
    return true;
}

// The serial bus runs at about 19200 baud: ten bits a byte. A drive
// answers a frame a little after its last byte, and takes longer before
// COMPLETE, as it would to find the sector.
enum { SIO_BYTE = 932, SIO_ACK_DELAY = 1800, SIO_COMPLETE_DELAY = 4000 };

// An ATX disk is read as the drive would read it. The head steps to the
// track (about 20 ms a track), the disk turns until the first header with
// the sector's number comes round, and the sector passes under the head:
// which of two copies of a sector is read depends on when it is asked
// for. A sector the track does not have is given up on after two turns.
static const double ATX_UNIT = CPU_HZ * 8e-6;  // cycles in 8 us
static const double ATX_STEP = CPU_HZ * 0.020;

// The controller's errors: lost data, CRC error, record not found.
static bool atx_error(uint8_t fdc) { return fdc & 0x1C; }

// Reads sector `n` from cycle `t`, into `buf`, leaving the controller's
// status in d->fdc. Returns the cycle it has been read by.
static uint64_t atx_read(Disk *d, int n, uint64_t t, uint8_t *buf) {
    int track = (n - 1) / d->spt, number = (n - 1) % d->spt + 1;
    t += (uint64_t)(abs(track - d->head) * ATX_STEP);
    d->head = track;
    uint64_t turn = (uint64_t)(ATX_TURN * ATX_UNIT);
    int now = (int)((double)(t % turn) / ATX_UNIT), best = -1, wait = 0;
    for (int i = d->track[track]; i < d->track[track + 1]; i++) {
        int w = (d->atx[i].pos - now + ATX_TURN) % ATX_TURN;
        if (d->atx[i].number == number && (best < 0 || w < wait)) {
            best = i;
            wait = w;
        }
    }
    memset(buf, 0, d->sector_size);
    if (best < 0) {
        d->fdc = 0x10;
        return t + 2 * turn;
    }
    const AtxSector *e = &d->atx[best];
    // A long sector (lost data) leaves DRQ set too.
    d->fdc = e->status | (e->status & 0x04 ? 0x02 : 0);
    if (e->data) memcpy(buf, d->data + e->data, d->sector_size);
    // Weak bits read differently each time.
    uint32_t x = (uint32_t)t | 1;
    for (int i = e->weak < 0 ? d->sector_size : e->weak; i < d->sector_size; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
    // FM takes 64 us a byte, MFM (enhanced and double density) 32.
    int bytes = d->sector_size * (d->spt == 18 && d->sector_size == 128 ? 8 : 4);
    return t + (uint64_t)((wait + bytes) * ATX_UNIT);
}

// SIO's own variables on page zero, which programs look at after a call:
// STATUS, and CHKSUM, the checksum of the last frame (the data, or the
// command frame when there is none). Copy protection checks it.
enum { STATUS = 0x30, CHKSUM = 0x31 };

static int sio_finish(Machine *m, uint8_t status, uint8_t chksum) {
    m->ram[DSTATS] = status;
    m->ram[STATUS] = status;
    m->ram[CHKSUM] = chksum;
    trap_return(m, status);
    return SIO_DONE;
}

// The checksum of the command frame for the call in the device control block.
static uint8_t command_checksum(const Machine *m) {
    uint8_t f[4] = {(uint8_t)(m->ram[DDEVIC] + m->ram[DUNIT] - 1), m->ram[DCOMND], m->ram[DAUX1], m->ram[DAUX2]};
    return sio_checksum(f, 4);
}

// SIOV takes the request from the device control block and returns with
// the status in DSTATS and Y, and N set on an error. The transfer takes no
// time, except a sector read from an ATX disk: that takes as long as the
// drive and the bus would, and is tried a second time after an error, as
// SIO does.
int machine_sio(Machine *m) {
    if (!in_os(m, SIOV)) return SIO_NOT_MINE;
    Sio *s = &m->sio;
    if (s->busy) {
        if (m->clock < s->ready) {
            mark_used(m, s->drive);
            return SIO_WAIT;
        }
        s->busy = false;
        sio_send(m, s->buf, s->len);
        return sio_finish(m, s->status, sio_checksum(s->buf, s->len));
    }
    uint8_t status = SIO_TIMEOUT, chksum = command_checksum(m);
    if (m->ram[DDEVIC] == 0x60) {
        status = tape_sio(m, &chksum);
    } else {
        int unit = m->ram[DDEVIC] + m->ram[DUNIT] - 1;
        if (unit < 0x31 || unit >= 0x31 + DRIVES) return SIO_NOT_MINE;
        Disk *d = &m->disk[unit - 0x31];
        int n = word(m->ram + DAUX1);
        if (d->atx && m->ram[DCOMND] == 'R' && n >= 1 && n <= d->sectors) {
            // The command frame and ACK, the read, COMPLETE and the data.
            mark_used(m, unit - 0x31);
            uint64_t t = m->clock;
            for (int attempt = 0; attempt < 2; attempt++) {
                t += 5 * SIO_BYTE + SIO_ACK_DELAY;
                t = atx_read(d, n, t, s->buf) + (uint64_t)(d->sector_size + 2) * SIO_BYTE;
                if (!atx_error(d->fdc)) break;
            }
            s->busy = true;
            s->drive = unit - 0x31;
            s->ready = t;
            s->status = atx_error(d->fdc) ? SIO_DEVICE_ERROR : SIO_OK;
            s->len = d->sector_size;
            return SIO_WAIT;
        }
        if (d->data) {
            mark_used(m, unit - 0x31);
            uint8_t cmd = m->ram[DCOMND], buf[512] = {0};
            int len;
            uint16_t from = word(m->ram + DBUFLO);
            int in = disk_data_in(d, cmd, n);
            for (int i = 0; i < in; i++) buf[i] = sio_peek(m, (uint16_t)(from + i));
            if (in) chksum = sio_checksum(buf, in);
            status = disk_command(d, cmd, n, buf, &len);
            if (len) chksum = sio_checksum(buf, len);
            sio_send(m, buf, len);
        }
    }
    return sio_finish(m, status, chksum);
}

static uint8_t sio_checksum(const uint8_t *p, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; i++) {
        sum += p[i];
        sum = (sum & 0xFF) + (sum >> 8);
    }
    return (uint8_t)sum;
}

// Starts a reply: `ack` then, unless it is NAK, the status and `len` bytes
// of data with their checksum.
static void sio_reply(Machine *m, uint8_t ack, uint8_t status, const uint8_t *data, int len) {
    Sio *s = &m->sio;
    s->nout = s->sent = 0;
    s->out[s->nout++] = ack;
    if (ack == 'A' && status) {
        s->out[s->nout++] = status == SIO_OK ? 'C' : 'E';
        if (len) {
            memcpy(s->out + s->nout, data, len);
            s->nout += len;
            s->out[s->nout++] = sio_checksum(data, len);
        }
    }
    // The first byte comes after the frame's last has gone out.
    s->at = m->pokey.serout_at + SIO_ACK_DELAY;
    s->complete = SIO_COMPLETE_DELAY;
    serin_schedule(m);
}

// A whole command frame: device, command, two bytes of argument, checksum.
static void sio_command(Machine *m) {
    Sio *s = &m->sio;
    const uint8_t *f = s->in;
    if (f[0] < 0x31 || f[0] >= 0x31 + DRIVES) return;  // another device's
    Disk *d = &m->disk[f[0] - 0x31];
    if (!d->data || sio_checksum(f, 4) != f[4]) return;  // no answer
    s->drive = f[0] - 0x31;
    mark_used(m, s->drive);
    int n = f[2] | f[3] << 8, in = disk_data_in(d, f[1], n);
    if (in) {
        memcpy(s->cmd, f, 4);
        s->want = in + 1;
        sio_reply(m, 'A', 0, NULL, 0);
        return;
    }
    uint8_t buf[512];
    int len;
    if (d->atx && f[1] == 'R' && n >= 1 && n <= d->sectors) {
        // The drive starts on the sector once it has sent ACK.
        uint64_t ack = m->pokey.serout_at + SIO_ACK_DELAY;
        uint64_t done = atx_read(d, n, ack, buf);
        sio_reply(m, 'A', atx_error(d->fdc) ? SIO_DEVICE_ERROR : SIO_OK, buf, d->sector_size);
        if (done - ack > s->complete) s->complete = done - ack;
        return;
    }
    uint8_t status = disk_command(d, f[1], n, buf, &len);
    if (status == SIO_NAK)
        sio_reply(m, 'N', 0, NULL, 0);
    else
        sio_reply(m, 'A', status, buf, len);
}

// The data frame of a write: its bytes and their checksum.
static void sio_data(Machine *m) {
    Sio *s = &m->sio;
    int len = s->want - 1;
    s->want = 0;
    if (sio_checksum(s->in, len) != s->in[len]) {
        sio_reply(m, 'N', 0, NULL, 0);
        return;
    }
    Disk *d = &m->disk[s->drive];
    if (!d->data) return;
    uint8_t buf[512];
    memcpy(buf, s->in, len);
    int out;
    uint8_t status = disk_command(d, s->cmd[1], s->cmd[2] | s->cmd[3] << 8, buf, &out);
    sio_reply(m, 'A', status, buf, out);
}

// The command line, PIA CB2: low when PBCTL's bits 3-5 are 110.
static void sio_command_line(Machine *m, bool low) {
    Sio *s = &m->sio;
    if (low && !s->command) {
        // A new command: whatever was under way is abandoned.
        s->nin = s->want = 0;
        s->nout = s->sent = 0;
        s->at = ~(uint64_t)0;
        serin_schedule(m);
    }
    if (!low && s->command && s->want) s->nin = 0;  // a write's data frame comes next
    s->command = low;
}

void machine_serout(Machine *m, uint8_t v) {
    Sio *s = &m->sio;
    if (s->command) {
        if (s->nin < 5) s->in[s->nin++] = v;
        if (s->nin == 5) {
            s->nin = 6;  // ignores the rest until the line is raised again
            sio_command(m);
        }
    } else if (s->want) {
        mark_used(m, s->drive);
        s->in[s->nin++] = v;
        if (s->nin == s->want) {
            s->nin = 0;
            sio_data(m);
        }
    }
}

uint8_t machine_serin(Machine *m) {
    Sio *s = &m->sio;
    uint64_t at = m->pokey.serin_at;
    uint8_t v = m->pokey.serin;
    if (s->at == at) {
        s->at = ~(uint64_t)0;
        if (s->sent < s->nout) {
            mark_used(m, s->drive);
            v = s->out[s->sent++];
            if (s->sent < s->nout) s->at = at + (s->sent == 1 ? s->complete : SIO_BYTE);
        }
    } else if (m->tape.at == at) {
        v = tape_next(m);
    }
    serin_schedule(m);
    return v;
}

// Maps 8K of ROM at page `first`, for the CPU and ANTIC.
static void map_rom(Machine *m, int first, uint8_t *rom) {
    for (int p = first; p < first + 0x20; p++) {
        m->rmap[p] = m->amap[p] = rom + (p - first) * 256;
        m->wmap[p] = m->sink;
    }
}

// The cartridge's two 8K windows, $8000-$9FFF and $A000-$BFFF, each NULL
// when it shows nothing there. A MegaCart maps a 16K bank at $8000, or
// none when bit 7 of its register is set. A SIC! maps the lower half of
// its bank at $8000 when bit 5 is set and the upper half at $A000 unless
// bit 6 is.
static void cart_windows(const Machine *m, uint8_t **lo, uint8_t **hi) {
    *lo = *hi = NULL;
    if (!m->cart) return;
    size_t mask = m->cart_size / 0x4000 - 1;
    uint8_t *bank = m->cart + (m->cart_bank & mask) * 0x4000;
    switch (m->cart_type) {
    case CART_STANDARD:
        if (m->cart_size == 0x4000) *lo = m->cart;
        *hi = m->cart + m->cart_size - 0x2000;
        break;
    case CART_MEGACART:
        if (!(m->cart_bank & 0x80)) {
            *lo = bank;
            *hi = bank + 0x2000;
        }
        break;
    case CART_SIC:
        if (m->cart_bank & 0x20) *lo = bank;
        if (!(m->cart_bank & 0x40)) *hi = bank + 0x2000;
        break;
    }
}

// PORTB on the XL: bit 0 enables the OS ROM, bit 1 low enables BASIC, bit
// 7 low maps the self test ROM at $5000. As on the 130XE, bits 2-3 pick a
// 16K bank of the extra RAM for $4000-$7FFF, which the CPU sees when bit 4
// is low and ANTIC when bit 5 is. Lines set as inputs read high.
void machine_update_memory_map(Machine *m) {
    uint8_t pb = (m->pia.orb & m->pia.ddrb) | ~m->pia.ddrb;
    for (int p = 0; p < 256; p++) m->rmap[p] = m->wmap[p] = m->amap[p] = m->ram + p * 256;
    uint8_t *bank = m->xram + ((pb >> 2) & 3) * 0x4000;
    for (int p = 0x40; p < 0x80; p++) {
        if (!(pb & 0x10)) m->rmap[p] = m->wmap[p] = bank + (p - 0x40) * 256;
        if (!(pb & 0x20)) m->amap[p] = bank + (p - 0x40) * 256;
    }
    if (pb & 0x01) {
        map_rom(m, 0xC0, m->os);
        map_rom(m, 0xE0, m->os + 0x2000);
        if (!(pb & 0x80)) {
            for (int p = 0x50; p < 0x58; p++) {
                m->rmap[p] = m->amap[p] = m->os + 0x1000 + (p - 0x50) * 256;
                m->wmap[p] = m->sink;
            }
        }
    }
    uint8_t *lo, *hi;
    cart_windows(m, &lo, &hi);
    if (lo) map_rom(m, 0x80, lo);
    if (hi) {
        map_rom(m, 0xA0, hi);
    } else if (m->has_basic && !(pb & 0x02)) {
        map_rom(m, 0xA0, m->basic);
    }
    m->rd5 = hi != NULL;
    for (int p = 0xD0; p < 0xD8; p++) m->rmap[p] = m->wmap[p] = m->amap[p] = NULL;
}

// The cartridge control area, $D500-$D5FF. A MegaCart takes a write
// anywhere in it as its bank register; a SIC! has its register at
// $D500-$D51F, readable too. Flash programming is not emulated.
static uint8_t cartctl_read(Machine *m, uint16_t addr) {
    if (m->cart && m->cart_type == CART_SIC && (addr & 0xE0) == 0) return m->cart_bank;
    return 0xFF;
}

static void cartctl_write(Machine *m, uint16_t addr, uint8_t v) {
    if (!m->cart) return;
    if (m->cart_type == CART_MEGACART || (m->cart_type == CART_SIC && (addr & 0xE0) == 0)) {
        m->cart_bank = v;
        machine_update_memory_map(m);
    }
}

static uint8_t pia_read(Machine *m, int reg) {
    Pia *p = &m->pia;
    switch (reg) {
    case 0:
        if (p->pactl & 4) {
            uint8_t in = (uint8_t)~(m->stick[0] | m->stick[1] << 4);
            return (p->ora & p->ddra) | (in & ~p->ddra);
        }
        return p->ddra;
    case 1:
        if (p->pbctl & 4) return (p->orb & p->ddrb) | ~p->ddrb;
        return p->ddrb;
    case 2: return p->pactl & 0x3F;
    default: return p->pbctl & 0x3F;
    }
}

static void pia_write(Machine *m, int reg, uint8_t v) {
    Pia *p = &m->pia;
    switch (reg) {
    case 0:
        if (p->pactl & 4)
            p->ora = v;
        else
            p->ddra = v;
        break;
    case 1:
        if (p->pbctl & 4)
            p->orb = v;
        else
            p->ddrb = v;
        machine_update_memory_map(m);
        break;
    case 2:
        p->pactl = v;
        tape_motor(m, (v & 0x38) == 0x30);
        break;
    default:
        p->pbctl = v;
        sio_command_line(m, (v & 0x38) == 0x30);
        break;
    }
}

uint8_t io_read(Machine *m, uint16_t addr) {
    switch (addr >> 8) {
    case 0xD0: return gtia_read(m, addr & 0x1F);
    case 0xD2: return pokey_read(m, addr & 0x0F);
    case 0xD3: return pia_read(m, addr & 0x03);
    case 0xD4: return antic_read(m, addr & 0x0F);
    case 0xD5: return cartctl_read(m, addr);
    default: return 0xFF;
    }
}

void io_write(Machine *m, uint16_t addr, uint8_t v) {
    switch (addr >> 8) {
    case 0xD0: gtia_write(m, addr & 0x1F, v); break;
    case 0xD2: pokey_write(m, addr & 0x0F, v); break;
    case 0xD3: pia_write(m, addr & 0x03, v); break;
    case 0xD4: antic_write(m, addr & 0x0F, v); break;
    case 0xD5: cartctl_write(m, addr, v); break;
    default: break;
    }
}

void machine_end_line(Machine *m) {
    gtia_render(m, LINE_CLOCKS);
    m->gtia.cc = 0;
    m->x = 0;
    if (++m->line == LINES_PER_FRAME) m->line = 0;
    if (m->line >= FRAME_TOP && m->line < FRAME_BOTTOM) m->frame_hires[m->line - FRAME_TOP] = false;
    antic_start_line(m);
    if (m->line == FRAME_BOTTOM) m->frame_done = true;
}

void machine_run_frame(Machine *m) {
    if (m->hold_start) m->hold_start--;
    m->frame_done = false;
    cpu_run(m);
    pokey_sync(m);
}

static void reset_chips(Machine *m) {
    memset(&m->antic, 0, sizeof m->antic);
    memset(&m->gtia, 0, sizeof m->gtia);
    memset(&m->pia, 0, sizeof m->pia);
    memset(&m->sio, 0, sizeof m->sio);
    m->sio.at = ~(uint64_t)0;
    m->tape.motor = false;
    m->tape.start = m->tape.at = ~(uint64_t)0;
    pokey_reset(m);
    m->nmi = false;
    m->irq = false;
    m->wsync = false;
    machine_update_memory_map(m);
}

// A bank-switched cartridge starts in bank 0; the RESET key leaves its
// register alone, as the XL's cartridge slot has no reset line.
void machine_cold_reset(Machine *m) {
    memset(m->ram, 0, sizeof m->ram);
    memset(m->xram, 0, sizeof m->xram);
    m->cart_bank = 0;
    m->xex_offset = 0;
    m->xex_loading = m->xex != NULL;
    m->x = 0;
    m->line = 0;
    // A tape starts from the beginning, with START held so the OS boots it,
    // as on power on with START down.
    m->tape.record = m->tape.pos = 0;
    m->hold_start = m->tape.data ? 30 : 0;
    m->cas.iocb = -1;
    reset_chips(m);
    memset(m->dma, 0, sizeof m->dma);
    memset(&m->cpu, 0, sizeof m->cpu);
    cpu_reset(m);
}

// The XL's RESET key resets the CPU and the PIA (so the OS ROM comes back);
// RAM is kept and the OS decides what survives. An open XEX, a disk in
// D1 or a tape boots afresh.
void machine_warm_reset(Machine *m) {
    if (m->xex || m->disk[0].data || m->tape.data) {
        machine_cold_reset(m);
        return;
    }
    memset(&m->pia, 0, sizeof m->pia);
    sio_command_line(m, false);
    tape_motor(m, false);
    machine_update_memory_map(m);
    m->antic.nmien = 0;
    m->nmi = false;
    m->wsync = false;
    cpu_reset(m);
}

void machine_key(Machine *m, uint8_t code, bool down) { pokey_key(m, code, down); }

void machine_break_key(Machine *m) { pokey_break(m); }
