#import "SallyView.h"

#import <Carbon/Carbon.h>

#import "Keyboard.h"

@implementation SallyView {
    uint32_t _joystick;       // JoyBits from arrows and Option
    uint32_t _console;
    uint32_t _commandKeys;     // keys held through Cmd-1 to Cmd-4, as Command bits
    NSMutableDictionary<NSNumber *, NSNumber *> *_pressed;  // Mac key code -> Atari code
    NSEventModifierFlags _flags;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    _pressed = [NSMutableDictionary dictionary];
    [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}

- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.backgroundColor = NSColor.blackColor.CGColor;
    return layer;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)updateDrawableSize {
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    if (!self.window) return;
    layer.contentsScale = self.window.backingScaleFactor;
    NSSize size = [self convertSizeToBacking:self.bounds.size];
    if (size.width >= 1 && size.height >= 1) layer.drawableSize = size;
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self updateDrawableSize];
}

#pragma mark Keyboard

- (void)setJoystick:(uint32_t)bit down:(BOOL)down {
    _joystick = down ? _joystick | bit : _joystick & ~bit;
    self.emulator.keyboardJoystick = _joystick;
}

- (void)setConsole:(uint32_t)bit down:(BOOL)down {
    _console = down ? _console | bit : _console & ~bit;
    self.emulator.consoleKeys = _console;
}

// Returns YES if the key is one the window handles itself.
- (BOOL)special:(NSEvent *)event down:(BOOL)down {
    switch (special_key(event)) {
    case SpecialUp: [self setJoystick:JoyUp down:down]; return YES;
    case SpecialDown: [self setJoystick:JoyDown down:down]; return YES;
    case SpecialLeft: [self setJoystick:JoyLeft down:down]; return YES;
    case SpecialRight: [self setJoystick:JoyRight down:down]; return YES;
    case SpecialBreak:
        if (down && !event.isARepeat) [self.emulator breakKey];
        return YES;
    default: return NO;
    }
}

- (void)keyDown:(NSEvent *)event {
    [NSCursor setHiddenUntilMouseMoves:YES];
    if ([self special:event down:YES]) return;
    if (event.isARepeat) return;  // the Atari OS repeats keys itself
    int code = atari_key_code(event);
    if (code < 0) return;
    _pressed[@(event.keyCode)] = @(code);
    [self.emulator keyDown:code];
}

- (void)keyUp:(NSEvent *)event {
    if ([self special:event down:NO]) return;
    NSNumber *code = _pressed[@(event.keyCode)];
    if (!code) return;
    [_pressed removeObjectForKey:@(event.keyCode)];
    [self.emulator keyUp:code.intValue];
}

- (void)flagsChanged:(NSEvent *)event {
    NSEventModifierFlags flags = event.modifierFlags;
    // Option is the joystick's fire button.
    [self setJoystick:JoyFire down:(flags & NSEventModifierFlagOption) != 0];
    self.emulator.shiftHeld = (flags & NSEventModifierFlagShift) != 0;
    // Caps Lock toggles on the Mac; each change is one press of the Atari's
    // CAPS key.
    if ((flags ^ _flags) & NSEventModifierFlagCapsLock) {
        [self.emulator keyDown:0x3C];
        [self.emulator keyUp:0x3C];
    }
    _flags = flags;
}

// HELP's bit in _commandKeys, after the console keys' 1, 2 and 4.
static const uint32_t CommandHelp = 8;

- (BOOL)commandShortcut:(NSEvent *)event {
    uint32_t bit;
    switch (event.keyCode) {
    case kVK_ANSI_1: bit = 1; break;
    case kVK_ANSI_2: bit = 2; break;
    case kVK_ANSI_3: bit = 4; break;
    case kVK_ANSI_4: bit = CommandHelp; break;
    default: return NO;
    }
    if (event.type == NSEventTypeKeyDown) {
        if (!(event.modifierFlags & NSEventModifierFlagCommand)) return NO;
        if (_commandKeys & bit) return YES;  // a repeat
        _commandKeys |= bit;
        if (bit == CommandHelp) {
            int code = 0x11 | ((event.modifierFlags & NSEventModifierFlagControl) ? 0x80 : 0);
            _pressed[@(event.keyCode)] = @(code);
            [self.emulator keyDown:code];
        } else {
            [self setConsole:bit down:YES];
        }
        return YES;
    }
    // The release counts even if Command was let go first.
    if (!(_commandKeys & bit)) return NO;
    _commandKeys &= ~bit;
    if (bit == CommandHelp)
        [self keyUp:event];
    else
        [self setConsole:bit down:NO];
    return YES;
}

- (void)pressConsole:(uint32_t)bit {
    [self setConsole:bit down:YES];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self setConsole:bit down:NO];
    });
}

- (void)releaseKeys {
    for (NSNumber *code in _pressed.allValues) [self.emulator keyUp:code.intValue];
    [_pressed removeAllObjects];
    _joystick = 0;
    _console = 0;
    _commandKeys = 0;
    self.emulator.keyboardJoystick = 0;
    self.emulator.consoleKeys = 0;
    self.emulator.shiftHeld = NO;
}

#pragma mark Drag and drop

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [sender.draggingPasteboard canReadObjectForClasses:@[ NSURL.class ]
                                                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL *> *urls =
        [sender.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                 options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (!urls.count || !self.openFile) return NO;
    self.openFile(urls.firstObject);
    return YES;
}

@end
