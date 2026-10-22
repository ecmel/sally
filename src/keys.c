#include "keys.h"

int atari_char_code(int ch) {
    static const char letters[26] = {
        0x3F, 0x15, 0x12, 0x3A, 0x2A, 0x38, 0x3D, 0x39, 0x0D, 0x01, 0x05, 0x00, 0x25,
        0x23, 0x08, 0x0A, 0x2F, 0x28, 0x3E, 0x2D, 0x0B, 0x10, 0x2E, 0x16, 0x2B, 0x17,
    };
    static const char digits[10] = {0x32, 0x1F, 0x1E, 0x1A, 0x18, 0x1D, 0x1B, 0x33, 0x35, 0x30};
    if (ch >= 'a' && ch <= 'z') return letters[ch - 'a'];
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'] | 0x40;
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    switch (ch) {
    case ' ': return 0x21;
    case ',': return 0x20;
    case '.': return 0x22;
    case '/': return 0x26;
    case ';': return 0x02;
    case '-': return 0x0E;
    case '=': return 0x0F;
    case '+': return 0x06;
    case '*': return 0x07;
    case '<': return 0x36;
    case '>': return 0x37;
    case '!': return 0x5F;
    case '"': return 0x5E;
    case '#': return 0x5A;
    case '$': return 0x58;
    case '%': return 0x5D;
    case '&': return 0x5B;
    case '\'': return 0x73;
    case '@': return 0x75;
    case '(': return 0x70;
    case ')': return 0x72;
    case '[': return 0x60;
    case ']': return 0x62;
    case ':': return 0x42;
    case '?': return 0x66;
    case '_': return 0x4E;
    case '|': return 0x4F;
    case '\\': return 0x46;
    case '^': return 0x47;
    default: return -1;
    }
}
