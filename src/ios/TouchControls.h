// The on-screen controls, drawn over the picture: the fire button on the
// left, a joystick on the right, the console keys and a menu button. A
// touch anywhere on the left half fires and anywhere on the right half
// steers, so a thumb need not land on the drawn control.

#import <UIKit/UIKit.h>

@interface TouchControls : UIView

// Called whenever what is held changes: JoyBits, and the console keys
// (START 1, SELECT 2, OPTION 4).
@property (nonatomic, copy) void (^changed)(uint32_t joystick, uint32_t console);
// Hides the joystick and fire button, for when a game controller is
// connected. The console keys and the menu stay.
@property (nonatomic) BOOL padHidden;
// The button in the top left corner; its menu is the owner's to set.
@property (nonatomic, readonly) UIButton *menuButton;

// Lets go of everything, as when the app leaves the screen.
- (void)releaseAll;

@end
