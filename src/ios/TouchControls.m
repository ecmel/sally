#import "TouchControls.h"

#import "Emulator.h"

static const CGFloat Margin = 16;

// What a touch does, kept for as long as it lasts: the half of the screen
// or the key it started on decides, wherever it moves.
enum { RoleNone, RoleStick, RoleFire, RoleConsole = 0x100 };  // RoleConsole | key bit

// The joystick for a touch at `v` from the stick's center: eight sectors of
// 45 degrees, nothing inside the dead zone.
static uint32_t direction(CGPoint v, CGFloat dead) {
    if (hypot(v.x, v.y) < dead) return 0;
    const CGFloat t = 0.4142;  // tan 22.5 degrees
    uint32_t bits = 0;
    if (-v.y > t * fabs(v.x)) bits |= JoyUp;
    if (v.y > t * fabs(v.x)) bits |= JoyDown;
    if (-v.x > t * fabs(v.y)) bits |= JoyLeft;
    if (v.x > t * fabs(v.y)) bits |= JoyRight;
    return bits;
}

static UIColor *fill(BOOL pressed) {
    return [UIColor colorWithWhite:1 alpha:pressed ? 0.45 : 0.12];
}

static UIColor *stroke(void) {
    return [UIColor colorWithWhite:1 alpha:0.45];
}

// One drawn control. It takes no touches: the overlay routes them.
@interface ControlShape : UIView
@property (nonatomic, copy) NSString *title;  // a key; nil for the stick or fire
@property (nonatomic) BOOL stick;
@property (nonatomic) BOOL pressed;
@property (nonatomic) uint32_t joystick;  // the stick's direction
@end

@implementation ControlShape

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.opaque = NO;
    self.userInteractionEnabled = NO;
    self.contentMode = UIViewContentModeRedraw;
    return self;
}

- (void)setPressed:(BOOL)pressed {
    if (pressed == _pressed) return;
    _pressed = pressed;
    [self setNeedsDisplay];
}

- (void)setJoystick:(uint32_t)joystick {
    if (joystick == _joystick) return;
    _joystick = joystick;
    [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect {
    CGRect b = CGRectInset(self.bounds, 1, 1);
    if (self.title) {
        UIBezierPath *pill = [UIBezierPath bezierPathWithRoundedRect:b cornerRadius:b.size.height / 2];
        [fill(self.pressed) setFill];
        [pill fill];
        [stroke() setStroke];
        [pill stroke];
        NSDictionary *attrs = @{
            NSFontAttributeName : [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold],
            NSForegroundColorAttributeName : [UIColor colorWithWhite:1 alpha:0.8],
        };
        CGSize size = [self.title sizeWithAttributes:attrs];
        [self.title drawAtPoint:CGPointMake(CGRectGetMidX(b) - size.width / 2, CGRectGetMidY(b) - size.height / 2)
                 withAttributes:attrs];
        return;
    }
    UIBezierPath *base = [UIBezierPath bezierPathWithOvalInRect:b];
    [fill(self.pressed && !self.stick) setFill];
    [base fill];
    [stroke() setStroke];
    [base stroke];
    if (!self.stick) return;

    // A chevron at each of the four directions, and a knob pushed the way
    // the stick points.
    CGFloat r = b.size.width / 2;
    CGPoint c = CGPointMake(CGRectGetMidX(b), CGRectGetMidY(b));
    UIBezierPath *marks = [UIBezierPath bezierPath];
    for (int i = 0; i < 4; i++) {
        CGFloat a = i * M_PI_2, s = r * 0.1;
        CGPoint tip = CGPointMake(c.x + cos(a) * r * 0.86, c.y + sin(a) * r * 0.86);
        CGPoint back = CGPointMake(tip.x - cos(a) * s, tip.y - sin(a) * s);
        [marks moveToPoint:CGPointMake(back.x - sin(a) * s, back.y + cos(a) * s)];
        [marks addLineToPoint:tip];
        [marks addLineToPoint:CGPointMake(back.x + sin(a) * s, back.y - cos(a) * s)];
    }
    marks.lineWidth = 2;
    marks.lineCapStyle = kCGLineCapRound;
    marks.lineJoinStyle = kCGLineJoinRound;
    [marks stroke];

    CGFloat dx = (self.joystick & JoyRight ? 1 : 0) - (self.joystick & JoyLeft ? 1 : 0);
    CGFloat dy = (self.joystick & JoyDown ? 1 : 0) - (self.joystick & JoyUp ? 1 : 0);
    CGFloat len = hypot(dx, dy);
    if (len > 0) {
        dx /= len;
        dy /= len;
    }
    CGFloat k = r * 0.4;
    CGPoint knob = CGPointMake(c.x + dx * r * 0.4, c.y + dy * r * 0.4);
    UIBezierPath *path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(knob.x - k, knob.y - k, 2 * k, 2 * k)];
    [fill(self.joystick != 0) setFill];
    [path fill];
    [path stroke];
}

@end

@implementation TouchControls {
    ControlShape *_stick, *_fire;
    NSArray<ControlShape *> *_keys;  // START, SELECT, OPTION
    NSMapTable<UITouch *, NSNumber *> *_roles;
    NSMutableArray<UITouch *> *_stickTouches;  // the last one steers
    __weak UITouch *_anchorTouch;  // the touch _anchor belongs to
    CGPoint _anchor;
    uint32_t _joystick, _console;
    UIImpactFeedbackGenerator *_tap;
    UISelectionFeedbackGenerator *_turn;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.multipleTouchEnabled = YES;
    _roles = [NSMapTable strongToStrongObjectsMapTable];
    _stickTouches = [NSMutableArray array];
    _tap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    _turn = [UISelectionFeedbackGenerator new];

    _stick = [ControlShape new];
    _stick.stick = YES;
    _fire = [ControlShape new];
    NSMutableArray *keys = [NSMutableArray array];
    for (NSString *title in @[ @"START", @"SELECT", @"OPTION" ]) {
        ControlShape *key = [ControlShape new];
        key.title = title;
        [keys addObject:key];
        [self addSubview:key];
    }
    _keys = keys;
    [self addSubview:_stick];
    [self addSubview:_fire];

    UIButtonConfiguration *config = [UIButtonConfiguration plainButtonConfiguration];
    config.image = [UIImage systemImageNamed:@"ellipsis.circle"
                           withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:22]];
    config.baseForegroundColor = [UIColor colorWithWhite:1 alpha:0.6];
    _menuButton = [UIButton buttonWithConfiguration:config primaryAction:nil];
    _menuButton.showsMenuAsPrimaryAction = YES;
    [self addSubview:_menuButton];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect b = self.bounds;
    UIEdgeInsets safe = self.safeAreaInsets;
    CGFloat left = safe.left + Margin, right = b.size.width - safe.right - Margin;
    CGFloat top = safe.top + Margin, bottom = b.size.height - safe.bottom - Margin;

    CGFloat r = MIN(90, b.size.height * 0.2);
    // The two the same size, mirrored.
    CGFloat f = r * 0.75;
    _stick.frame = CGRectMake(right - r - f, bottom - r - f, 2 * f, 2 * f);
    _fire.frame = CGRectMake(left + r - f, bottom - r - f, 2 * f, 2 * f);

    CGFloat w = 84, h = 32;
    CGFloat gap = 12, x = CGRectGetMidX(b) - (_keys.count * (w + gap) - gap) / 2;
    for (NSUInteger i = 0; i < _keys.count; i++) _keys[i].frame = CGRectMake(x + i * (w + gap), bottom - h, w, h);
    _menuButton.frame = CGRectMake(left - 8, top - 6, 44, 44);
}

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsLayout];
}

- (void)setPadHidden:(BOOL)padHidden {
    _padHidden = padHidden;
    _stick.hidden = _fire.hidden = padHidden;
    if (!padHidden) return;
    for (UITouch *touch in _roles.keyEnumerator.allObjects) {
        int role = [_roles objectForKey:touch].intValue;
        if (role == RoleStick || role == RoleFire) [_roles setObject:@(RoleNone) forKey:touch];
    }
    [_stickTouches removeAllObjects];
    [self update];
}

#pragma mark Touches

- (int)roleAt:(CGPoint)p {
    for (NSUInteger i = 0; i < _keys.count; i++)
        if (CGRectContainsPoint(CGRectInset(_keys[i].frame, -8, -8), p)) return RoleConsole | 1 << i;
    if (self.padHidden) return RoleNone;
    return p.x < self.bounds.size.width / 2 ? RoleFire : RoleStick;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        int role = [self roleAt:[touch locationInView:self]];
        [_roles setObject:@(role) forKey:touch];
        if (role == RoleStick) [_stickTouches addObject:touch];
    }
    [self update];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self update];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        [_roles removeObjectForKey:touch];
        [_stickTouches removeObject:touch];
    }
    [self update];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}

- (void)releaseAll {
    [_roles removeAllObjects];
    [_stickTouches removeAllObjects];
    [self update];
}

// Works out what is held from the touches, and reports any change.
- (void)update {
    uint32_t joystick = 0, console = 0;
    for (UITouch *touch in _roles) {
        int role = [_roles objectForKey:touch].intValue;
        if (role == RoleFire) joystick |= JoyFire;
        if (role & RoleConsole) console |= role & 7;
    }
    UITouch *steer = _stickTouches.lastObject;
    if (steer) {
        // Steering is measured from an anchor that starts at the stick's
        // center and follows the thumb on a short leash, so it acts as a
        // digital stick: wherever the thumb has wandered, a short move
        // turns it the other way.
        CGPoint p = [steer locationInView:self];
        CGFloat r = _stick.bounds.size.width / 2, leash = r * 0.3;
        if (steer != _anchorTouch) {
            _anchorTouch = steer;
            _anchor = CGPointMake(CGRectGetMidX(_stick.frame), CGRectGetMidY(_stick.frame));
        }
        CGFloat dx = p.x - _anchor.x, dy = p.y - _anchor.y, d = hypot(dx, dy);
        if (d > leash) {
            _anchor.x += dx * (1 - leash / d);
            _anchor.y += dy * (1 - leash / d);
        }
        joystick |= direction(CGPointMake(p.x - _anchor.x, p.y - _anchor.y), r * 0.12);
    } else {
        _anchorTouch = nil;
    }
    if (joystick == _joystick && console == _console) return;

    uint32_t dir = joystick & ~JoyFire;
    if (dir && dir != (_joystick & ~JoyFire)) [_turn selectionChanged];
    if ((joystick & ~_joystick & JoyFire) || (console & ~_console)) [_tap impactOccurred];
    _joystick = joystick;
    _console = console;

    _stick.joystick = dir;
    _fire.pressed = (joystick & JoyFire) != 0;
    for (NSUInteger i = 0; i < _keys.count; i++) _keys[i].pressed = (console >> i) & 1;
    if (self.changed) self.changed(joystick, console);
}

@end
