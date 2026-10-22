#import "Keyboard.h"

#import <Carbon/Carbon.h>

#include "keys.h"

SpecialKey special_key(NSEvent *event) {
    switch (event.keyCode) {
    case kVK_UpArrow: return SpecialUp;
    case kVK_DownArrow: return SpecialDown;
    case kVK_LeftArrow: return SpecialLeft;
    case kVK_RightArrow: return SpecialRight;
    case kVK_ForwardDelete: return SpecialBreak;  // BREAK sits right of Delete
    default: return SpecialNone;
    }
}

// The key left of 1. ISO keyboards report it as kVK_ISO_Section and give
// kVK_ANSI_Grave to the key left of Z instead.
static bool left_of_one(NSEvent *event) {
    if (event.keyCode == kVK_ISO_Section) return true;
    if (event.keyCode != kVK_ANSI_Grave) return false;
    int64_t type = CGEventGetIntegerValueField(event.CGEvent, kCGKeyboardEventKeyboardType);
    return KBGetLayoutType((short)type) != kKeyboardISO;
}

int atari_key_code(NSEvent *event) {
    NSEventModifierFlags flags = event.modifierFlags;
    int ctrl = (flags & NSEventModifierFlagControl) ? 0x80 : 0;
    int shift = (flags & NSEventModifierFlagShift) ? 0x40 : 0;
    switch (event.keyCode) {
    case kVK_Return:
    case kVK_ANSI_KeypadEnter: return 0x0C | shift | ctrl;
    case kVK_Delete: return 0x34 | shift | ctrl;
    case kVK_Tab: return 0x2C | shift | ctrl;
    case kVK_Escape: return 0x1C | shift | ctrl;
    case kVK_Home: return 0x36 | 0x40;            // Clear
    case kVK_CapsLock: return 0x3C | shift | ctrl;
    default: break;
    }
    if (left_of_one(event)) return 0x27 | shift | ctrl;  // the Inverse key
    NSString *s = [event charactersByApplyingModifiers:flags & NSEventModifierFlagShift];
    if (s.length != 1) return -1;
    int code = atari_char_code([s characterAtIndex:0]);
    return code < 0 ? -1 : code | ctrl;
}
