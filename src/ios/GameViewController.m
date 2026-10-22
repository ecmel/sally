#import "GameViewController.h"

#import <GameController/GameController.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "Emulator.h"
#import "TouchControls.h"
#include "keys.h"

static NSString *const LastCartridgeKey = @"LastCartridge";

static NSURL *documents(void) {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
}

static BOOL is_cartridge(NSURL *url) {
    NSString *ext = url.pathExtension.lowercaseString;
    return [ext isEqualToString:@"rom"] || [ext isEqualToString:@"car"] || [ext isEqualToString:@"bin"];
}

// The Metal layer the emulator draws into, sized in physical pixels so the
// picture keeps its whole-pixel scale.
@interface ScreenView : UIView
@end

@implementation ScreenView

+ (Class)layerClass {
    return CAMetalLayer.class;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat scale = self.window.windowScene.screen.nativeScale;
    if (scale <= 0) return;
    self.contentScaleFactor = scale;
    CGSize size = self.bounds.size;
    if (size.width >= 1 && size.height >= 1)
        ((CAMetalLayer *)self.layer).drawableSize = CGSizeMake(size.width * scale, size.height * scale);
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self setNeedsLayout];
}

@end

@interface GameViewController () <UIDocumentPickerDelegate>
@end

@implementation GameViewController {
    ScreenView *_screen;
    TouchControls *_controls;
    Emulator *_emulator;
    NSString *_cartridge;  // the file name in Documents, or nil
    BOOL _userPaused;
    BOOL _started;
    NSString *_pendingError;  // shown once the view is on screen

    // Input from each source, combined for the emulator.
    uint32_t _touchJoystick, _keyJoystick;
    uint32_t _touchConsole, _keyConsole, _padConsole;
    NSMutableDictionary<NSNumber *, NSNumber *> *_pressedKeys;     // HID usage -> Atari code
    NSMutableDictionary<NSNumber *, NSNumber *> *_commandKeys;     // HID usage -> console bit
}

- (void)loadView {
    UIView *view = [UIView new];
    view.backgroundColor = UIColor.blackColor;
    _screen = [ScreenView new];
    _controls = [TouchControls new];
    [view addSubview:_screen];
    [view addSubview:_controls];
    self.view = view;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    _pressedKeys = [NSMutableDictionary dictionary];
    _commandKeys = [NSMutableDictionary dictionary];
    _emulator = [[Emulator alloc] initWithLayer:(CAMetalLayer *)_screen.layer];
    if (!_emulator) {
        _pendingError = @"Metal is not available.";
        return;
    }

    __weak GameViewController *weakSelf = self;
    _controls.changed = ^(uint32_t joystick, uint32_t console) {
        GameViewController *strong = weakSelf;
        if (!strong) return;
        strong->_touchJoystick = joystick;
        strong->_touchConsole = console;
        [strong updateInput];
    };
    _controls.menuButton.menu = [UIMenu menuWithChildren:@[
        [UIDeferredMenuElement elementWithUncachedProvider:^(void (^completion)(NSArray<UIMenuElement *> *)) {
            completion([weakSelf menuElements]);
        }],
    ]];

    NSString *last = [NSUserDefaults.standardUserDefaults stringForKey:LastCartridgeKey];
    if (last) [self openCartridgeNamed:last];

    for (NSNotificationName name in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification ])
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(controllersChanged:)
                                                   name:name
                                                 object:nil];
    [self controllersChanged:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _screen.frame = self.view.bounds;
    _controls.frame = self.view.bounds;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
    if (!_started && _emulator) {
        [_emulator start];
        _started = YES;
    }
    if (_pendingError) {
        NSString *message = _pendingError;
        _pendingError = nil;
        [self showError:message title:nil];
    }
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

// Swipes from the edges go to the game first, so a thumb sliding off the
// joystick does not pull down Control Center.
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return UIRectEdgeAll;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (void)setSuspended:(BOOL)suspended {
    _suspended = suspended;
    if (suspended) [self releaseAll];
    _emulator.paused = _userPaused || suspended;
}

- (void)releaseAll {
    [_controls releaseAll];
    for (NSNumber *code in _pressedKeys.allValues) [_emulator keyUp:code.intValue];
    [_pressedKeys removeAllObjects];
    [_commandKeys removeAllObjects];
    _keyJoystick = _keyConsole = _padConsole = 0;
    _emulator.shiftHeld = NO;
    _emulator.padJoystick = 0;
    [self updateInput];
}

- (void)updateInput {
    _emulator.keyboardJoystick = _touchJoystick | _keyJoystick;
    _emulator.consoleKeys = _touchConsole | _keyConsole | _padConsole;
}

- (void)showError:(NSString *)message title:(NSString *)title {
    if (!self.view.window) {
        _pendingError = message;
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark Cartridges

- (NSArray<NSURL *> *)cartridges {
    NSArray<NSURL *> *urls = [NSFileManager.defaultManager contentsOfDirectoryAtURL:documents()
                                                         includingPropertiesForKeys:nil
                                                                            options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                              error:nil];
    urls = [urls filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSURL *url, NSDictionary *bindings) {
                     return is_cartridge(url);
                 }]];
    return [urls sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
}

// Loads `data` and remembers it as `name`. Returns NO after showing why
// it failed.
- (BOOL)loadData:(NSData *)data name:(NSString *)name {
    NSString *message = [_emulator loadCartridge:data];
    if (message) {
        [self showError:message title:[NSString stringWithFormat:@"“%@” cannot be opened.", name]];
        return NO;
    }
    _cartridge = name;
    [NSUserDefaults.standardUserDefaults setObject:name forKey:LastCartridgeKey];
    return YES;
}

- (void)openCartridgeNamed:(NSString *)name {
    NSURL *url = [documents() URLByAppendingPathComponent:name];
    NSData *data = [NSData dataWithContentsOfURL:url];
    if (!data) {
        // Deleted in the Files app since.
        [NSUserDefaults.standardUserDefaults removeObjectForKey:LastCartridgeKey];
        return;
    }
    [self loadData:data name:name];
}

- (void)openURL:(NSURL *)url {
    [self loadViewIfNeeded];
    if (!_emulator) return;
    BOOL scoped = [url startAccessingSecurityScopedResource];
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&error];
    if (scoped) [url stopAccessingSecurityScopedResource];
    NSString *name = url.lastPathComponent;
    if (!data) {
        [self showError:error.localizedDescription title:[NSString stringWithFormat:@"“%@” cannot be opened.", name]];
        return;
    }
    if (![self loadData:data name:name]) return;
    // Keep a copy, unless this is already one.
    NSURL *copy = [documents() URLByAppendingPathComponent:name];
    if (![copy.URLByResolvingSymlinksInPath isEqual:url.URLByResolvingSymlinksInPath])
        [data writeToURL:copy options:NSDataWritingAtomic error:nil];
}

- (void)importCartridge {
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in @[ @"rom", @"car", @"bin" ]) {
        UTType *type = [UTType typeWithFilenameExtension:ext];
        if (type) [types addObject:type];
    }
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types
                                                                                                         asCopy:YES];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (urls.count) [self openURL:urls.firstObject];
}

- (void)ejectCartridge {
    [_emulator ejectCartridge];
    _cartridge = nil;
    [NSUserDefaults.standardUserDefaults removeObjectForKey:LastCartridgeKey];
}

#pragma mark Menu

- (NSArray<UIMenuElement *> *)menuElements {
    __weak GameViewController *weakSelf = self;
    NSMutableArray<UIMenuElement *> *carts = [NSMutableArray array];
    for (NSURL *url in self.cartridges) {
        NSString *name = url.lastPathComponent;
        UIAction *action = [UIAction actionWithTitle:url.URLByDeletingPathExtension.lastPathComponent
                                               image:nil
                                          identifier:nil
                                             handler:^(UIAction *a) {
                                                 [weakSelf openCartridgeNamed:name];
                                             }];
        if ([name isEqualToString:_cartridge]) action.state = UIMenuElementStateOn;
        [carts addObject:action];
    }
    UIMenu *library = [UIMenu menuWithTitle:@"Cartridges"
                                      image:nil
                                 identifier:nil
                                    options:UIMenuOptionsDisplayInline
                                   children:carts];

    UIAction *import = [UIAction actionWithTitle:@"Import Cartridge…"
                                           image:[UIImage systemImageNamed:@"folder"]
                                      identifier:nil
                                         handler:^(UIAction *a) {
                                             [weakSelf importCartridge];
                                         }];
    UIAction *eject = [UIAction actionWithTitle:@"Eject"
                                          image:[UIImage systemImageNamed:@"eject"]
                                     identifier:nil
                                        handler:^(UIAction *a) {
                                            [weakSelf ejectCartridge];
                                        }];
    if (!_cartridge) eject.attributes = UIMenuElementAttributesDisabled;
    UIMenu *file = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[
        import, eject
    ]];

    UIAction *pause = [UIAction actionWithTitle:@"Pause"
                                          image:[UIImage systemImageNamed:@"pause"]
                                     identifier:nil
                                        handler:^(UIAction *a) {
                                            GameViewController *strong = weakSelf;
                                            if (!strong) return;
                                            strong->_userPaused = !strong->_userPaused;
                                            strong->_emulator.paused = strong->_userPaused || strong.suspended;
                                        }];
    if (_userPaused) pause.state = UIMenuElementStateOn;
    UIAction *reset = [UIAction actionWithTitle:@"Reset"
                                          image:[UIImage systemImageNamed:@"arrow.counterclockwise"]
                                     identifier:nil
                                        handler:^(UIAction *a) {
                                            [weakSelf.emulator warmReset];
                                        }];
    UIAction *power = [UIAction actionWithTitle:@"Power Cycle"
                                          image:[UIImage systemImageNamed:@"power"]
                                     identifier:nil
                                        handler:^(UIAction *a) {
                                            [weakSelf.emulator coldReset];
                                        }];
    UIMenu *machine = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[
        pause, reset, power
    ]];
    // The menu opens downwards from the top corner, so the machine's items
    // come first and the list, which can grow long, last.
    return @[ machine, file, library ];
}

- (Emulator *)emulator {
    return _emulator;
}

#pragma mark Game controllers

- (void)controllersChanged:(NSNotification *)notification {
    __weak GameViewController *weakSelf = self;
    BOOL any = NO;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        any = YES;
        pad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
            [weakSelf padChanged:gamepad];
        };
    }
#if TARGET_OS_SIMULATOR
    // The simulator always offers a gamepad of its own.
    any = NO;
#endif
    _controls.padHidden = any;
}

// Menu is START and Options SELECT, so a game starts without reaching for
// the screen.
- (void)padChanged:(GCExtendedGamepad *)pad {
    float x = pad.dpad.xAxis.value + pad.leftThumbstick.xAxis.value;
    float y = pad.dpad.yAxis.value + pad.leftThumbstick.yAxis.value;
    uint32_t bits = 0;
    if (y > 0.5) bits |= JoyUp;
    if (y < -0.5) bits |= JoyDown;
    if (x < -0.5) bits |= JoyLeft;
    if (x > 0.5) bits |= JoyRight;
    if (pad.buttonA.pressed || pad.buttonB.pressed || pad.rightTrigger.pressed) bits |= JoyFire;
    _emulator.padJoystick = bits;
    _padConsole = (pad.buttonMenu.pressed ? 1 : 0) | (pad.buttonOptions.pressed ? 2 : 0);
    [self updateInput];
}

#pragma mark Hardware keyboard

// The same keys as on the Mac: arrows and Option for the joystick, Command
// with 1 to 4 for START, SELECT, OPTION and HELP, Forward Delete for BREAK.

- (BOOL)canBecomeFirstResponder {
    return YES;
}

static uint32_t joystick_bit(UIKeyboardHIDUsage usage) {
    switch (usage) {
    case UIKeyboardHIDUsageKeyboardUpArrow: return JoyUp;
    case UIKeyboardHIDUsageKeyboardDownArrow: return JoyDown;
    case UIKeyboardHIDUsageKeyboardLeftArrow: return JoyLeft;
    case UIKeyboardHIDUsageKeyboardRightArrow: return JoyRight;
    case UIKeyboardHIDUsageKeyboardLeftAlt:
    case UIKeyboardHIDUsageKeyboardRightAlt: return JoyFire;
    default: return 0;
    }
}

// The Atari keyboard code for a key (bit 6 Shift, bit 7 Control), or -1.
static int atari_key(UIKey *key) {
    int ctrl = (key.modifierFlags & UIKeyModifierControl) ? 0x80 : 0;
    int shift = (key.modifierFlags & UIKeyModifierShift) ? 0x40 : 0;
    switch (key.keyCode) {
    case UIKeyboardHIDUsageKeyboardReturnOrEnter:
    case UIKeyboardHIDUsageKeypadEnter: return 0x0C | shift | ctrl;
    case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: return 0x34 | shift | ctrl;
    case UIKeyboardHIDUsageKeyboardTab: return 0x2C | shift | ctrl;
    case UIKeyboardHIDUsageKeyboardEscape: return 0x1C | shift | ctrl;
    case UIKeyboardHIDUsageKeyboardHome: return 0x36 | 0x40;  // Clear
    case UIKeyboardHIDUsageKeyboardCapsLock: return 0x3C | shift | ctrl;
    case UIKeyboardHIDUsageKeyboardGraveAccentAndTilde: return 0x27 | shift | ctrl;  // Inverse, left of 1
    default: break;
    }
    // With Control held the typed character is a control code; the plain
    // one names the key.
    NSString *s = ctrl ? key.charactersIgnoringModifiers : key.characters;
    if (s.length != 1) return -1;
    int code = atari_char_code([s characterAtIndex:0]);
    return code < 0 ? -1 : code | ctrl | (ctrl ? shift : 0);
}

// Returns NO for a key left to the system.
- (BOOL)keyDown:(UIKey *)key {
    UIKeyboardHIDUsage usage = key.keyCode;
    uint32_t bit = joystick_bit(usage);
    if (bit) {
        _keyJoystick |= bit;
        [self updateInput];
        return YES;
    }
    switch (usage) {
    case UIKeyboardHIDUsageKeyboardLeftShift:
    case UIKeyboardHIDUsageKeyboardRightShift: _emulator.shiftHeld = YES; return YES;
    case UIKeyboardHIDUsageKeyboardDeleteForward: [_emulator breakKey]; return YES;
    default: break;
    }
    if (_pressedKeys[@(usage)] || _commandKeys[@(usage)]) return YES;  // a repeat
    if (key.modifierFlags & UIKeyModifierCommand) {
        uint32_t console;
        switch (usage) {
        case UIKeyboardHIDUsageKeyboard1: console = 1; break;
        case UIKeyboardHIDUsageKeyboard2: console = 2; break;
        case UIKeyboardHIDUsageKeyboard3: console = 4; break;
        case UIKeyboardHIDUsageKeyboard4: {
            int code = 0x11 | ((key.modifierFlags & UIKeyModifierControl) ? 0x80 : 0);  // HELP
            _pressedKeys[@(usage)] = @(code);
            [_emulator keyDown:code];
            return YES;
        }
        default: return NO;
        }
        _commandKeys[@(usage)] = @(console);
        _keyConsole |= console;
        [self updateInput];
        return YES;
    }
    int code = atari_key(key);
    if (code < 0) return NO;
    _pressedKeys[@(usage)] = @(code);
    [_emulator keyDown:code];
    return YES;
}

- (BOOL)keyUp:(UIKey *)key {
    UIKeyboardHIDUsage usage = key.keyCode;
    uint32_t bit = joystick_bit(usage);
    if (bit) {
        _keyJoystick &= ~bit;
        [self updateInput];
        return YES;
    }
    switch (usage) {
    case UIKeyboardHIDUsageKeyboardLeftShift:
    case UIKeyboardHIDUsageKeyboardRightShift: _emulator.shiftHeld = NO; return YES;
    case UIKeyboardHIDUsageKeyboardDeleteForward: return YES;
    default: break;
    }
    NSNumber *console = _commandKeys[@(usage)];
    if (console) {
        [_commandKeys removeObjectForKey:@(usage)];
        _keyConsole &= ~console.unsignedIntValue;
        [self updateInput];
        return YES;
    }
    NSNumber *code = _pressedKeys[@(usage)];
    if (!code) return NO;
    [_pressedKeys removeObjectForKey:@(usage)];
    [_emulator keyUp:code.intValue];
    return YES;
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    NSMutableSet *rest = [NSMutableSet set];
    for (UIPress *press in presses)
        if (!press.key || ![self keyDown:press.key]) [rest addObject:press];
    if (rest.count) [super pressesBegan:rest withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    NSMutableSet *rest = [NSMutableSet set];
    for (UIPress *press in presses)
        if (!press.key || ![self keyUp:press.key]) [rest addObject:press];
    if (rest.count) [super pressesEnded:rest withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    NSMutableSet *rest = [NSMutableSet set];
    for (UIPress *press in presses)
        if (!press.key || ![self keyUp:press.key]) [rest addObject:press];
    if (rest.count) [super pressesCancelled:rest withEvent:event];
}

@end
