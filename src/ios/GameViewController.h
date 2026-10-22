// The one screen: the picture, the on-screen controls over it, the
// cartridges, game controllers and a hardware keyboard.
//
// Cartridges live in the app's Documents folder, which the Files app shows
// as Sally's. Opening one from elsewhere copies it there, and the menu lists
// them all.

#import <UIKit/UIKit.h>

@interface GameViewController : UIViewController

// Opens a cartridge from the Files app or another app.
- (void)openURL:(NSURL *)url;

// Pauses the machine and lets go of every control while the app is not
// active.
@property (nonatomic) BOOL suspended;

@end
