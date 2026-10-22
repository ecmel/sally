// Characters to the Atari keyboard, for both frontends: a printable key
// goes to the Atari key that types the same character.

#ifndef SALLY_KEYS_H
#define SALLY_KEYS_H

// The Atari keyboard code that types `ch` (bit 6 Shift), or -1 if none
// does.
int atari_char_code(int ch);

#endif
