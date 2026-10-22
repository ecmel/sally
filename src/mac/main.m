// Sally: an Atari 800 XL emulator. The application: its window and menus,
// opening cartridges and game controllers.

#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "Emulator.h"
#import "SallyView.h"
#include "machine.h"

static NSString *const LastCartridgeKey = @"LastCartridge";

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    NSWindow *_window;
    SallyView *_view;
    Emulator *_emulator;
    NSURL *_pending;  // a file opened before the window exists
    BOOL _launched;
}

#pragma mark Menus

static NSMenuItem *item(NSMenu *menu, NSString *title, SEL action, NSString *key, NSEventModifierFlags mods) {
    NSMenuItem *i = [menu addItemWithTitle:title action:action keyEquivalent:key];
    i.keyEquivalentModifierMask = mods;
    return i;
}

static NSMenu *submenu(NSMenu *bar, NSString *title) {
    NSMenuItem *top = [bar addItemWithTitle:title action:nil keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    top.submenu = menu;
    return menu;
}

- (void)buildMenus {
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSMenu *bar = [NSMenu new];

    NSMenu *app = submenu(bar, @"Sally");
    item(app, @"About Sally", @selector(showAbout:), @"", 0);
    [app addItem:NSMenuItem.separatorItem];
    item(app, @"Hide Sally", @selector(hide:), @"h", cmd);
    item(app, @"Hide Others", @selector(hideOtherApplications:), @"h", cmd | NSEventModifierFlagOption);
    item(app, @"Show All", @selector(unhideAllApplications:), @"", 0);
    [app addItem:NSMenuItem.separatorItem];
    item(app, @"Quit Sally", @selector(terminate:), @"q", cmd);

    NSMenu *file = submenu(bar, @"File");
    item(file, @"Open…", @selector(openDocument:), @"o", cmd);
    item(file, @"Eject", @selector(ejectCartridge:), @"e", cmd);
    [file addItem:NSMenuItem.separatorItem];
    item(file, @"Close Window", @selector(performClose:), @"w", cmd);

    NSMenu *machine = submenu(bar, @"Machine");
    item(machine, @"Pause", @selector(togglePause:), @"p", cmd);
    [machine addItem:NSMenuItem.separatorItem];
    // The metal keys, in order, on Command and the digits.
    item(machine, @"Start", @selector(startKey:), @"1", cmd);
    item(machine, @"Select", @selector(selectKey:), @"2", cmd);
    item(machine, @"Option", @selector(optionKey:), @"3", cmd);
    item(machine, @"Help", @selector(helpKey:), @"4", cmd);
    item(machine, @"Reset", @selector(warmReset:), @"5", cmd);
    item(machine, @"Power Cycle", @selector(coldReset:), @"6", cmd);
    [machine addItem:NSMenuItem.separatorItem];
    // Plastic keys, on plain keys of their own.
    item(machine, @"Break", @selector(breakKey:), @"", 0);
    item(machine, @"Inverse Key", @selector(inverseKey:), @"", 0);

    NSMenu *view = submenu(bar, @"View");
    item(view, @"Fit Window to Picture", @selector(fitWindow:), @"0", cmd);
    item(view, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", cmd | NSEventModifierFlagControl);

    NSMenu *window = submenu(bar, @"Window");
    item(window, @"Minimize", @selector(performMiniaturize:), @"m", cmd);
    item(window, @"Zoom", @selector(performZoom:), @"", 0);
    NSApp.windowsMenu = window;

    NSApp.mainMenu = bar;
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    SEL action = menuItem.action;
    if (action == @selector(togglePause:)) menuItem.state = _emulator.paused;
    return YES;
}

// The standard panel, without the build number in parentheses: with
// semantic versions it only repeats CFBundleShortVersionString.
- (void)showAbout:(id)sender {
    [NSApp orderFrontStandardAboutPanelWithOptions:@{NSAboutPanelOptionVersion: @""}];
}

#pragma mark Launch

- (void)applicationWillFinishLaunching:(NSNotification *)notification {
    // One window, no tabs: keeps AppKit from adding tab items to View.
    NSWindow.allowsAutomaticWindowTabbing = NO;
    [self buildMenus];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSScreen *screen = NSScreen.mainScreen;
    CGFloat scale = screen.backingScaleFactor;
    NSRect visible = screen.visibleFrame;
    int clock, line;
    [Emulator scaleForSize:CGSizeMake(visible.size.width * scale * 0.9, (visible.size.height - 32) * scale * 0.9)
                     clock:&clock
                      line:&line];
    NSRect frame = NSMakeRect(0, 0, FRAME_CLOCKS * clock / scale, VIEW_HEIGHT * line / scale);

    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Sally";
    _window.delegate = self;
    _window.backgroundColor = NSColor.blackColor;
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.contentMinSize = NSMakeSize(FRAME_CLOCKS * 2 / scale, VIEW_HEIGHT / scale);
    _window.releasedWhenClosed = NO;

    _view = [[SallyView alloc] initWithFrame:frame];
    _window.contentView = _view;
    _emulator = [[Emulator alloc] initWithLayer:(CAMetalLayer *)_view.layer];
    if (!_emulator) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Metal is not available.";
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    _view.emulator = _emulator;
    __weak AppDelegate *weakSelf = self;
    _view.openFile = ^(NSURL *url) {
        [weakSelf openCartridge:url];
    };

    // `-control ADDRESS` opens the control socket.
    NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
    NSString *file = nil;
    for (NSUInteger i = 1; i < args.count; i++) {
        if ([args[i] isEqualToString:@"-control"] && i + 1 < args.count) {
            NSString *error = [_emulator listenOn:args[++i]];
            if (error) fprintf(stderr, "Sally: control socket: %s\n", error.UTF8String);
        } else if (![args[i] hasPrefix:@"-"] && !file) {
            file = args[i];
        }
    }
    _emulator.cartridgeChanged = ^(NSURL *url) {
        [weakSelf showCartridge:url];
    };

    // A file from the command line, from Finder, or the last one.
    NSURL *url = _pending;
    if (!url && file) url = [NSURL fileURLWithPath:file];
    if (!url) {
        NSString *last = [NSUserDefaults.standardUserDefaults stringForKey:LastCartridgeKey];
        if (last && [NSFileManager.defaultManager fileExistsAtPath:last]) url = [NSURL fileURLWithPath:last];
    }
    if (url) [self openCartridge:url];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];
    [_emulator start];
    _launched = YES;

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(controllersChanged:)
                                               name:GCControllerDidConnectNotification
                                             object:nil];
    [self controllersChanged:nil];

    // The Command shortcuts of START, SELECT, OPTION and HELP are held down
    // like the keys themselves; the menu only sees presses, so they are
    // caught here.
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp
                                          handler:^NSEvent *(NSEvent *event) {
                                              AppDelegate *strong = weakSelf;
                                              if (strong && event.window == strong->_window &&
                                                  [strong->_view commandShortcut:event])
                                                  return nil;
                                              return event;
                                          }];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls {
    if (!urls.count) return;
    if (_launched)
        [self openCartridge:urls.firstObject];
    else
        _pending = urls.firstObject;
}

#pragma mark Cartridges

- (void)openCartridge:(NSURL *)url {
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&error];
    NSString *message = data ? [_emulator loadCartridge:data] : error.localizedDescription;
    if (message) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = [NSString stringWithFormat:@"“%@” cannot be opened.", url.lastPathComponent];
        alert.informativeText = message;
        [alert runModal];
        return;
    }
    [self showCartridge:url];
}

// Shows the cartridge in the window and remembers it, or forgets it (nil).
- (void)showCartridge:(NSURL *)url {
    if (!url) {
        _window.title = @"Sally";
        _window.representedURL = nil;
        [NSUserDefaults.standardUserDefaults removeObjectForKey:LastCartridgeKey];
        return;
    }
    _window.representedURL = url;
    _window.title = url.URLByDeletingPathExtension.lastPathComponent;
    // The file's icon as its type defines it; the title bar may otherwise
    // keep a blank one cached from before Sally claimed the type.
    UTType *type = nil;
    [url getResourceValue:&type forKey:NSURLContentTypeKey error:nil];
    if (type) [_window standardWindowButton:NSWindowDocumentIconButton].image = [NSWorkspace.sharedWorkspace iconForContentType:type];
    [NSUserDefaults.standardUserDefaults setObject:url.path forKey:LastCartridgeKey];
    [NSDocumentController.sharedDocumentController noteNewRecentDocumentURL:url];
}

- (void)openDocument:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[
        [UTType typeWithFilenameExtension:@"rom"], [UTType typeWithFilenameExtension:@"car"],
        [UTType typeWithFilenameExtension:@"bin"]
    ];
    [panel beginSheetModalForWindow:_window
                  completionHandler:^(NSModalResponse result) {
                      if (result == NSModalResponseOK) [self openCartridge:panel.URL];
                  }];
}

- (void)ejectCartridge:(id)sender {
    [_emulator ejectCartridge];
    [self showCartridge:nil];
}

#pragma mark Machine

- (void)warmReset:(id)sender {
    [_emulator warmReset];
}

- (void)coldReset:(id)sender {
    [_emulator coldReset];
}

// Menu clicks on the console and special keys: a short press.
- (void)startKey:(id)sender {
    [_view pressConsole:1];
}

- (void)selectKey:(id)sender {
    [_view pressConsole:2];
}

- (void)optionKey:(id)sender {
    [_view pressConsole:4];
}

- (void)breakKey:(id)sender {
    [_emulator breakKey];
}

- (void)helpKey:(id)sender {
    [_emulator keyDown:0x11];
    [_emulator keyUp:0x11];
}

- (void)inverseKey:(id)sender {
    [_emulator keyDown:0x27];
    [_emulator keyUp:0x27];
}

- (void)togglePause:(id)sender {
    _emulator.paused = !_emulator.paused;
}

#pragma mark Window

// Shrinks the window to the whole-pixel picture it shows.
- (void)fitWindow:(id)sender {
    if (_window.styleMask & NSWindowStyleMaskFullScreen) return;
    CGFloat scale = _window.backingScaleFactor;
    NSSize size = _view.bounds.size;
    int clock, line;
    [Emulator scaleForSize:CGSizeMake(size.width * scale, size.height * scale) clock:&clock line:&line];
    NSRect frame = _window.frame;
    NSRect content = [_window contentRectForFrameRect:frame];
    CGFloat top = NSMaxY(frame);
    content.size = NSMakeSize(FRAME_CLOCKS * clock / scale, VIEW_HEIGHT * line / scale);
    frame = [_window frameRectForContentRect:content];
    frame.origin.y = top - frame.size.height;
    [_window setFrame:frame display:YES animate:YES];
}

- (void)windowDidResignKey:(NSNotification *)notification {
    [_view releaseKeys];
}

#pragma mark Game controllers

- (void)controllersChanged:(NSNotification *)notification {
    __weak AppDelegate *weakSelf = self;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        pad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
            [weakSelf padChanged:gamepad];
        };
    }
}

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
}

@end

// `-headless -control ADDRESS [FILE]`: no window, no sound, no Dock icon;
// the machine answers the control socket on the main thread.
static int run_headless(int argc, const char *argv[]) {
    const char *address = NULL, *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-control") && i + 1 < argc)
            address = argv[++i];
        else if (argv[i][0] != '-' && !file)
            file = argv[i];
    }
    if (!address) {
        fprintf(stderr, "Sally: -headless needs -control ADDRESS\n");
        return 2;
    }
    // Exits with the program that started it, so a trainer that dies does
    // not leave machines running. Ctrl-C in its terminal is for that
    // program, which may still need the machine to finish up.
    signal(SIGINT, SIG_IGN);
    static dispatch_source_t parent;
    parent = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)getppid(), DISPATCH_PROC_EXIT,
                                    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_event_handler(parent, ^{
        exit(0);
    });
    dispatch_resume(parent);

    Emulator *emulator = [[Emulator alloc] initHeadless];
    NSString *error = [emulator listenOn:@(address)];
    if (error) {
        fprintf(stderr, "Sally: control socket: %s\n", error.UTF8String);
        return 1;
    }
    if (file) {
        NSError *readError = nil;
        NSData *data = [NSData dataWithContentsOfFile:@(file) options:0 error:&readError];
        NSString *message = data ? [emulator loadCartridge:data] : readError.localizedDescription;
        if (message) {
            fprintf(stderr, "Sally: %s: %s\n", file, message.UTF8String);
            return 1;
        }
    }
    [emulator runHeadless];
    return 0;
}

int main(int argc, const char *argv[]) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-headless")) {
            @autoreleasepool {
                return run_headless(argc, argv);
            }
        }
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
