// Mac keys to the Atari keyboard. Printable keys go by the character they
// type (with Shift), so symbols land on the Atari key that has them; a few
// keys go by position.

#import <AppKit/AppKit.h>

// Keys the window handles itself rather than passing to the Atari keyboard.
typedef NS_ENUM(int, SpecialKey) {
    SpecialNone = 0,
    SpecialUp,
    SpecialDown,
    SpecialLeft,
    SpecialRight,
    SpecialBreak,
};

SpecialKey special_key(NSEvent *event);

// The Atari keyboard code for a key press (bit 6 Shift, bit 7 Control), or
// -1 if the key has none.
int atari_key_code(NSEvent *event);
