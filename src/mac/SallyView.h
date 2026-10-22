// The window's content: a CAMetalLayer the emulator draws into, keyboard
// input for the Atari, and cartridges dropped onto it.

#import <AppKit/AppKit.h>

#import "Emulator.h"

@interface SallyView : NSView

@property (nonatomic, strong) Emulator *emulator;
// Called with a file dropped onto the view.
@property (nonatomic, copy) void (^openFile)(NSURL *url);

// Releases every key, for when the window loses focus.
- (void)releaseKeys;

// Cmd-1 to Cmd-4 are START, SELECT, OPTION and HELP, held for as long as
// the keys are. AppKit does not send a view the release of a Command
// shortcut, so the app passes key events here first. Returns YES if the
// event was one of these.
- (BOOL)commandShortcut:(NSEvent *)event;

// A short press of a console key (1 START, 2 SELECT, 4 OPTION), for the menu.
- (void)pressConsole:(uint32_t)bit;

@end
