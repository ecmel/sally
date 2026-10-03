// Runs the machine on its own thread, paced by the display: a
// CAMetalDisplayLink asks for each frame shortly before the display shows
// it, the thread emulates as many Atari frames as are due, and draws the
// last one with Metal. Sound follows the video, with its rate nudged to
// keep the output buffer at a steady level.
//
// Methods are called on the main thread; the work is handed to the
// emulation thread, which alone touches the machine.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

// The lines shown: the frame buffer less 6 lines of overscan at the top
// and bottom, where games draw nothing. 228 lines fit 7 times in 1600
// pixels, the height of many Mac screens.
enum { VIEW_TOP = 6, VIEW_HEIGHT = 228 };

typedef NS_OPTIONS(uint32_t, JoyBits) {
    JoyUp = 1,
    JoyDown = 2,
    JoyLeft = 4,
    JoyRight = 8,
    JoyFire = 16,
};

@interface Emulator : NSObject <CAMetalDisplayLinkDelegate>

- (instancetype)initWithLayer:(CAMetalLayer *)layer;
// Starts the emulation thread; call once the layer is in a window.
- (void)start;

// Returns an error message, or nil.
- (NSString *)loadCartridge:(NSData *)data;
- (void)ejectCartridge;
- (void)coldReset;
- (void)warmReset;
- (void)breakKey;
- (void)keyDown:(int)code;
- (void)keyUp:(int)code;

@property (atomic) BOOL paused;
// Input, read once a frame. A press shorter than that still lasts two
// frames, so a quick tap is never lost between them.
@property (nonatomic) uint32_t keyboardJoystick;  // JoyBits
@property (nonatomic) uint32_t padJoystick;       // JoyBits
@property (nonatomic) uint32_t consoleKeys;       // START 1, SELECT 2, OPTION 4
@property (atomic) BOOL shiftHeld;

// The largest whole-pixel scale of the Atari picture that fits `pixels`:
// physical pixels per color clock and per scanline.
+ (void)scaleForSize:(CGSize)pixels clock:(int *)clock line:(int *)line;

@end
