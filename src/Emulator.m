#import "Emulator.h"

#import <Metal/Metal.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#import "Audio.h"
#include "control.h"
#include "machine.h"

// Width of a color clock relative to a scanline's height on an NTSC screen.
static const double PIXEL_ASPECT = 1.714;

static NSString *const shader_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Params { uint2 origin; uint clock; uint line; uint top; };\n"
     "vertex float4 vertex_main(uint id [[vertex_id]]) {\n"
     "    float2 p = float2((id << 1) & 2, id & 2);\n"
     "    return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
     "}\n"
     "fragment half4 fragment_main(float4 pos [[position]],\n"
     "                             device const uchar *frame [[buffer(0)]],\n"
     "                             constant uint *palette [[buffer(1)]],\n"
     "                             constant Params &p [[buffer(2)]]) {\n"
     "    uint2 q = uint2(pos.xy) - p.origin;\n"
     "    uint x = min(q.x * 2 / p.clock, 383u);\n"
     "    uint y = min(q.y / p.line + p.top, 239u);\n"
     "    uint rgb = palette[frame[y * 384 + x]];\n"
     "    return half4(half((rgb >> 16) & 255), half((rgb >> 8) & 255), half(rgb & 255), 255.0h) / 255.0h;\n"
     "}\n";

static void control_line(void *ctx, ControlClient *client, char *text);

// The control socket's frame format: each line of the frame buffer as GTIA
// put it out, a flag byte and then one color per color clock (0, 192
// bytes) or, on a high resolution line, one per half clock (1, 384 bytes).
enum { LINES_MAX = FRAME_HEIGHT * (1 + FRAME_WIDTH) };

static size_t encode_lines(const Machine *m, uint8_t *out) {
    uint8_t *p = out;
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        const uint8_t *row = m->frame + y * FRAME_WIDTH;
        *p++ = m->frame_hires[y];
        if (m->frame_hires[y]) {
            memcpy(p, row, FRAME_WIDTH);
            p += FRAME_WIDTH;
        } else {
            for (int x = 0; x < FRAME_CLOCKS; x++) *p++ = row[x * 2];
        }
    }
    return p - out;
}

typedef struct {
    uint32_t origin[2];
    uint32_t clock, line;
    uint32_t top;  // first frame buffer row shown
} Params;

// Decides how many Atari frames to run for each display refresh.
//
// When the display refreshes at a whole multiple of the Atari's frame rate
// (60 or 120 Hz, within 1%), frames lock to refreshes: each one stays on
// screen equally long and scrolling is even.
// The machine then runs at the display's rate, 0.13% fast at 60 Hz, and
// the sound's rate control absorbs the difference. Otherwise frames follow
// the clock.
typedef struct {
    double last;      // last target presentation time
    double period;    // estimated refresh period
    int samples;
    int refreshes;    // refreshes counted towards the next frame (locked)
    double owed;      // frames owed (following the clock)
} Pacer;

static int pacer_frames(Pacer *p, double t, double hz) {
    if (p->last == 0) {
        p->last = t;
        p->period = 1.0 / 60;
        return 1;
    }
    double dt = t - p->last;
    p->last = t;
    if (dt <= 0 || dt > 0.25) {
        // A stall: start over rather than catch up.
        p->refreshes = 0;
        p->owed = 0;
        return 1;
    }
    // Missed refreshes show up as whole multiples of the period; only
    // single ones refine the estimate.
    if (dt < p->period * 1.5) {
        p->period += (dt - p->period) * 0.1;
        p->samples++;
    } else if (p->samples < 10) {
        p->period = dt;
    }
    double per_frame = 1.0 / (p->period * hz);
    double whole = round(per_frame);
    if (p->samples > 20 && whole >= 1 && fabs(per_frame - whole) < 0.01 * whole) {
        p->owed = 0;
        p->refreshes += (int)lround(dt / p->period);
        int n = p->refreshes / (int)whole;
        p->refreshes -= n * (int)whole;
        return n > 4 ? 4 : n;
    }
    p->refreshes = 0;
    p->owed += dt * hz;
    int n = (int)p->owed;
    p->owed -= n;
    return n > 4 ? 4 : n;
}

@implementation Emulator {
    CAMetalLayer *_layer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLBuffer> _frames[3];
    id<MTLBuffer> _palette;
    int _slot;
    dispatch_semaphore_t _inflight;

    Machine *_m;
    AudioOut *_audio;
    double _audioTarget;
    double _fillAverage;
    double _rate;

    NSThread *_thread;
    CAMetalDisplayLink *_link;
    Pacer _pacer;
    // Without display refreshes (the display asleep, the window hidden), a
    // timer keeps the control socket and the machine running.
    NSTimer *_timer;
    Pacer _timerPacer;
    double _lastRefresh;   // when the display link last called
    id<NSObject> _activity;  // keeps App Nap off while the socket is open

    // Atari key presses waiting their turn (emulation thread only). Each
    // press is held for a few frames so programs that poll see it.
    struct {
        int code;
        bool down;
    } _keys[64];
    int _nkeys;
    int _keyHeld;  // the code held down, or -1
    int _holdFrames;

    // Joystick and console input, written on the main thread. `_pressed`
    // collects every bit that went down since the last frame (JoyBits,
    // then the console keys from bit 8); `_hold` keeps them down.
    _Atomic uint32_t _keyboardJoystick, _padJoystick, _consoleKeys, _pressed;
    uint8_t _hold[16];

    // The control socket and what it holds down (emulation thread only).
    Control *_control;
    uint32_t _remoteJoystick, _remoteConsole;
    bool _remoteShift;
    uint64_t _frameCount;  // frames emulated since launch
    uint8_t _lines[LINES_MAX];  // a frame in the socket's line format

    // Statistics, printed every second with SALLY_STATS=1.
    BOOL _stats;
    double _statsStart;
    int _statFrames, _statRefreshes, _statMissed;
    double _statEmuTime;
}

+ (void)scaleForSize:(CGSize)pixels clock:(int *)clock line:(int *)line {
    int pw = (int)pixels.width, ph = (int)pixels.height;
    for (int l = ph / VIEW_HEIGHT; l >= 1; l--) {
        double ideal = l * PIXEL_ASPECT;
        int c = (int)lround(ideal);
        // Prefer an even width so high resolution pixels stay whole.
        if (c & 1) {
            int e = fabs(c - 1 - ideal) < fabs(c + 1 - ideal) ? c - 1 : c + 1;
            if (e >= 2 && fabs(e - ideal) / ideal < 0.08) c = e;
        }
        if (c * FRAME_CLOCKS > pw) c = pw / FRAME_CLOCKS;
        if (c >= 1 && c >= ideal * 0.85) {
            *clock = c;
            *line = l;
            return;
        }
    }
    *clock = MAX(1, pw / FRAME_CLOCKS);
    *line = 1;
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer {
    if (!(self = [super init])) return nil;
    _layer = layer;
    _device = MTLCreateSystemDefaultDevice();
    if (!_device) return nil;
    _queue = [_device newCommandQueue];
    layer.device = _device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);

    NSError *error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:shader_source options:nil error:&error];
    if (!library) {
        NSLog(@"shader: %@", error);
        return nil;
    }
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    desc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!_pipeline) {
        NSLog(@"pipeline: %@", error);
        return nil;
    }
    for (int i = 0; i < 3; i++)
        _frames[i] = [_device newBufferWithLength:FRAME_WIDTH * FRAME_HEIGHT options:MTLResourceStorageModeShared];
    _inflight = dispatch_semaphore_create(3);

    _m = machine_new();
    _palette = [_device newBufferWithBytes:gtia_palette length:sizeof gtia_palette options:MTLResourceStorageModeShared];

    _audio = audio_open();
    _rate = _audio ? audio_rate(_audio) : 48000;
    _audioTarget = (_audio ? audio_device_frames(_audio) : 512) + _rate * 0.025;
    _fillAverage = _audioTarget;
    pokey_set_rate(_m, _rate);

    _keyHeld = -1;
    _stats = getenv("SALLY_STATS") != NULL;
    return self;
}

- (void)dealloc {
    control_close(_control);
    audio_close(_audio);
    machine_free(_m);
}

#pragma mark Input

- (uint32_t)keyboardJoystick {
    return atomic_load(&_keyboardJoystick);
}

- (void)setKeyboardJoystick:(uint32_t)bits {
    atomic_store(&_keyboardJoystick, bits);
    atomic_fetch_or(&_pressed, bits);
}

- (uint32_t)padJoystick {
    return atomic_load(&_padJoystick);
}

- (void)setPadJoystick:(uint32_t)bits {
    atomic_store(&_padJoystick, bits);
    atomic_fetch_or(&_pressed, bits);
}

- (uint32_t)consoleKeys {
    return atomic_load(&_consoleKeys);
}

- (void)setConsoleKeys:(uint32_t)bits {
    atomic_store(&_consoleKeys, bits);
    atomic_fetch_or(&_pressed, bits << 8);
}

#pragma mark Emulation thread

- (void)start {
    if (_thread) return;
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(threadMain) object:nil];
    _thread.name = @"Sally emulation";
    _thread.qualityOfService = NSQualityOfServiceUserInteractive;
    [_thread start];
}

- (void)threadMain {
    @autoreleasepool {
        _link = [[CAMetalDisplayLink alloc] initWithMetalLayer:_layer];
        _link.delegate = self;
        _link.preferredFrameLatency = 1;
        _link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
        [_link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
        _timer = [NSTimer timerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
        _timer.tolerance = 0;
        [NSRunLoop.currentRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    for (;;) {
        @autoreleasepool {
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:NSDate.distantFuture];
        }
    }
}

// Runs `block` on the emulation thread, or here before it has started.
- (void)perform:(dispatch_block_t)block wait:(BOOL)wait {
    if (!_thread || NSThread.currentThread == _thread) {
        block();
        return;
    }
    [self performSelector:@selector(runBlock:) onThread:_thread withObject:[block copy] waitUntilDone:wait
                    modes:@[ NSRunLoopCommonModes ]];
}

- (void)runBlock:(dispatch_block_t)block {
    block();
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    double hz = CPU_HZ / (LINES_PER_FRAME * CYCLES_PER_LINE);
    double t = update.targetPresentationTimestamp;
    double last = _pacer.last;
    _lastRefresh = CACurrentMediaTime();
    int n = pacer_frames(&_pacer, t, hz);
    if (self.paused) n = 0;

    if (_control) control_poll(_control, control_line, (__bridge void *)self);
    double start = CACurrentMediaTime();
    for (int i = 0; i < n; i++) [self runFrame];
    [self draw:update.drawable];

    if (_stats) {
        _statRefreshes++;
        _statFrames += n;
        _statEmuTime += CACurrentMediaTime() - start;
        if (last && t - last > _pacer.period * 1.5) _statMissed++;
        if (t - _statsStart >= 1) {
            if (_statsStart)
                fprintf(stderr,
                        "%d refreshes (%.2f Hz), %d frames, %d missed, %.3f ms per update, audio %d samples\n",
                        _statRefreshes, 1 / _pacer.period, _statFrames, _statMissed,
                        _statEmuTime * 1000 / _statRefreshes, _audio ? audio_buffered(_audio) : 0);
            _statsStart = t;
            _statRefreshes = _statFrames = _statMissed = 0;
            _statEmuTime = 0;
        }
    }
}

// The timer: answers the control socket whatever the display does, and runs
// the machine by the clock when the display link has stopped calling.
- (void)tick:(NSTimer *)timer {
    if (_control) control_poll(_control, control_line, (__bridge void *)self);
    double now = CACurrentMediaTime();
    if (now - _lastRefresh < 0.1) {
        _timerPacer.last = 0;  // start afresh when it is needed again
        return;
    }
    int n = pacer_frames(&_timerPacer, now, CPU_HZ / (LINES_PER_FRAME * CYCLES_PER_LINE));
    if (self.paused) n = 0;
    for (int i = 0; i < n; i++) [self runFrame];
}

- (void)feedKeys {
    if (_holdFrames > 0) _holdFrames--;
    while (_nkeys && _holdFrames == 0) {
        int code = _keys[0].code;
        bool down = _keys[0].down;
        memmove(_keys, _keys + 1, --_nkeys * sizeof _keys[0]);
        if (down) {
            machine_key(_m, (uint8_t)code, true);
            _keyHeld = code;
            _holdFrames = 3;
            break;
        }
        if (code == _keyHeld) {
            machine_key(_m, 0, false);
            _keyHeld = -1;
        }
    }
}

- (void)runFrame {
    Machine *m = _m;
    uint32_t held = self.keyboardJoystick | self.padJoystick | _remoteJoystick |
                    (self.consoleKeys | _remoteConsole) << 8;
    uint32_t pressed = atomic_exchange(&_pressed, 0);
    for (int b = 0; b < 16; b++) {
        if (pressed & (1u << b)) _hold[b] = 2;
        if (_hold[b]) {
            held |= 1u << b;
            _hold[b]--;
        }
    }
    m->stick[0] = held & 0x0F;
    m->trig[0] = (held & JoyFire) != 0;
    m->consol_keys = (held >> 8) & 7;
    [self feedKeys];
    if (_keyHeld < 0) m->pokey.shift_down = self.shiftHeld || _remoteShift;

    machine_run_frame(m);
    _frameCount++;
    if (_control && control_streaming(_control)) {
        char header[64];
        size_t n = encode_lines(m, _lines);
        snprintf(header, sizeof header, "frame %llu %d %zu\n", _frameCount, FRAME_HEIGHT, n);
        control_broadcast(_control, header, _lines, n);
    }

    if (_audio) {
        audio_write(_audio, m->pokey.audio, m->pokey.naudio);
        int fill = audio_buffered(_audio);
        if (fill < _audioTarget / 3) {
            // After a pause or a stall, refill at once with silence.
            static const float silence[2048];
            int pad = (int)_audioTarget - fill;
            audio_write(_audio, silence, pad > 2048 ? 2048 : pad);
            fill = audio_buffered(_audio);
            _fillAverage = fill;
        }
        _fillAverage += (fill - _fillAverage) * 0.05;
        double error = (_fillAverage - _audioTarget) / _audioTarget;
        double adjust = fmax(-0.005, fmin(0.005, error * 0.01));
        m->pokey.cps = CPU_HZ / _rate * (1 + adjust);
    }
    m->pokey.naudio = 0;
}

- (void)draw:(id<CAMetalDrawable>)drawable {
    if (!drawable) return;
    dispatch_semaphore_wait(_inflight, DISPATCH_TIME_FOREVER);
    id<MTLBuffer> frame = _frames[_slot];
    _slot = (_slot + 1) % 3;
    memcpy(frame.contents, _m->frame, sizeof _m->frame);

    id<MTLTexture> target = drawable.texture;
    int pw = (int)target.width, ph = (int)target.height;
    int clock, line;
    [Emulator scaleForSize:CGSizeMake(pw, ph) clock:&clock line:&line];
    int w = MIN(FRAME_CLOCKS * clock, pw), h = MIN(VIEW_HEIGHT * line, ph);
    Params params = {{(pw - w) / 2, (ph - h) / 2}, clock, line, VIEW_TOP};

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipeline];
    [encoder setViewport:(MTLViewport){params.origin[0], params.origin[1], w, h, 0, 1}];
    [encoder setFragmentBuffer:frame offset:0 atIndex:0];
    [encoder setFragmentBuffer:_palette offset:0 atIndex:1];
    [encoder setFragmentBytes:&params length:sizeof params atIndex:2];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands presentDrawable:drawable];
    dispatch_semaphore_t inflight = _inflight;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> b) {
        dispatch_semaphore_signal(inflight);
    }];
    [commands commit];
}

#pragma mark Commands

- (NSString *)loadCartridge:(NSData *)data {
    __block const char *err = NULL;
    [self perform:^{
        err = machine_load_cart(self->_m, data.bytes, data.length);
        if (!err) machine_cold_reset(self->_m);
    } wait:YES];
    return err ? @(err) : nil;
}

- (void)ejectCartridge {
    [self perform:^{
        machine_eject_cart(self->_m);
        machine_cold_reset(self->_m);
    } wait:NO];
}

- (void)coldReset {
    [self perform:^{
        machine_cold_reset(self->_m);
    } wait:NO];
}

- (void)warmReset {
    [self perform:^{
        machine_warm_reset(self->_m);
    } wait:NO];
}

- (void)breakKey {
    [self perform:^{
        machine_break_key(self->_m);
    } wait:NO];
}

- (void)queueKey:(int)code down:(bool)down {
    [self perform:^{
        if (self->_nkeys < (int)(sizeof self->_keys / sizeof self->_keys[0])) {
            self->_keys[self->_nkeys].code = code;
            self->_keys[self->_nkeys].down = down;
            self->_nkeys++;
        }
    } wait:NO];
}

- (void)keyDown:(int)code {
    [self queueKey:code down:true];
}

- (void)keyUp:(int)code {
    [self queueKey:code down:false];
}

#pragma mark Control socket

- (NSString *)listenOn:(NSString *)address {
    char err[256];
    Control *c = control_open(address.fileSystemRepresentation, err, sizeof err);
    if (!c) return @(err);
    control_close(_control);
    _control = c;
    // Programs drive the machine with its window hidden or the display
    // asleep; App Nap would slow the timer that keeps it going.
    if (!_activity)
        _activity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                              reason:@"Control socket"];
    return nil;
}

static void control_line(void *ctx, ControlClient *client, char *text) {
    [(__bridge Emulator *)ctx command:text client:client];
}

// Parses `s` as letters from `set` (each giving the bit of its position),
// or "-" for none. Returns -1 for anything else.
static int letter_bits(const char *s, const char *set) {
    if (!strcmp(s, "-")) return 0;
    int bits = 0;
    for (; *s; s++) {
        const char *p = strchr(set, toupper((unsigned char)*s));
        if (!p) return -1;
        bits |= 1 << (p - set);
    }
    return bits;
}

static bool parse_flag(const char *s, bool *on) {
    if (!strcmp(s, "0") || !strcmp(s, "1")) {
        *on = *s == '1';
        return true;
    }
    return false;
}

static bool parse_key(const char *s, int *code) {
    char *end;
    long v = strtol(s, &end, 16);
    if (!*s || *end || v < 0 || v > 0xFF) return false;
    *code = (int)v;
    return true;
}

// Runs one command from the control socket, on the emulation thread. The
// commands are listed in the README.
- (void)command:(char *)text client:(ControlClient *)client {
    Machine *m = _m;
    while (*text == ' ') text++;
    char *arg = strchr(text, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    } else {
        arg = text + strlen(text);
    }
    const char *cmd = text;
    int bits, code;
    bool on;

    if (!*cmd) return;
    if (!strcmp(cmd, "stick") && (bits = letter_bits(arg, "UDLR")) >= 0) {
        _remoteJoystick = (_remoteJoystick & JoyFire) | bits;
        atomic_fetch_or(&_pressed, bits);
    } else if (!strcmp(cmd, "fire") && parse_flag(arg, &on)) {
        _remoteJoystick = on ? _remoteJoystick | JoyFire : _remoteJoystick & ~JoyFire;
        if (on) atomic_fetch_or(&_pressed, JoyFire);
    } else if (!strcmp(cmd, "consol") && (bits = letter_bits(arg, "SEO")) >= 0) {
        _remoteConsole = bits;
        atomic_fetch_or(&_pressed, bits << 8);
    } else if (!strcmp(cmd, "shift") && parse_flag(arg, &on)) {
        _remoteShift = on;
    } else if (!strcmp(cmd, "key") && parse_key(arg, &code)) {
        [self queueKey:code down:true];
        [self queueKey:code down:false];
    } else if (!strcmp(cmd, "keydown") && parse_key(arg, &code)) {
        [self queueKey:code down:true];
    } else if (!strcmp(cmd, "keyup") && parse_key(arg, &code)) {
        [self queueKey:code down:false];
    } else if (!strcmp(cmd, "break") && !*arg) {
        machine_break_key(m);
    } else if (!strcmp(cmd, "reset") && !*arg) {
        machine_warm_reset(m);
    } else if (!strcmp(cmd, "power") && !*arg) {
        machine_cold_reset(m);
    } else if (!strcmp(cmd, "pause") && parse_flag(arg, &on)) {
        self.paused = on;
    } else if (!strcmp(cmd, "load") && *arg) {
        NSURL *url = [NSURL fileURLWithPath:@(arg)];
        NSError *error = nil;
        NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&error];
        const char *err = data ? machine_load_cart(m, data.bytes, data.length) : error.localizedDescription.UTF8String;
        if (err) {
            control_printf(client, "error %s\n", err);
            return;
        }
        machine_cold_reset(m);
        [self notifyCartridge:url];
    } else if (!strcmp(cmd, "eject") && !*arg) {
        machine_eject_cart(m);
        machine_cold_reset(m);
        [self notifyCartridge:nil];
    } else if (!strcmp(cmd, "frame") && !*arg) {
        size_t n = encode_lines(m, _lines);
        control_printf(client, "frame %llu %d %zu\n", _frameCount, FRAME_HEIGHT, n);
        control_send(client, _lines, n);
        return;
    } else if (!strcmp(cmd, "stream") && parse_flag(arg, &on)) {
        control_set_streaming(client, on);
    } else if (!strcmp(cmd, "palette") && !*arg) {
        uint8_t rgb[256 * 3];
        for (int i = 0; i < 256; i++) {
            rgb[i * 3] = gtia_palette[i] >> 16;
            rgb[i * 3 + 1] = gtia_palette[i] >> 8;
            rgb[i * 3 + 2] = gtia_palette[i];
        }
        control_printf(client, "palette %zu\n", sizeof rgb);
        control_send(client, rgb, sizeof rgb);
        return;
    } else if (!strcmp(cmd, "status") && !*arg) {
        control_printf(client, "status frame %llu paused %d cart %d\n", _frameCount, self.paused, m->cart != NULL);
        return;
    } else {
        control_printf(client, "error bad command: %s%s%s\n", cmd, *arg ? " " : "", arg);
        return;
    }
    control_printf(client, "ok\n");
}

- (void)notifyCartridge:(NSURL *)url {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.cartridgeChanged) self.cartridgeChanged(url);
    });
}

@end
