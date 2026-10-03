// Draws the app icon, an arcade ball-top joystick on sky blue, into an .iconset
// folder for iconutil.
//
//   makeicon DIR.iconset

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static CGColorRef rgb(int hex, CGFloat alpha) {
    return CGColorCreateSRGB((hex >> 16 & 255) / 255.0, (hex >> 8 & 255) / 255.0, (hex & 255) / 255.0, alpha);
}

static void fill(CGContextRef c, CGPathRef path, int hex) {
    CGColorRef color = rgb(hex, 1);
    CGContextAddPath(c, path);
    CGContextSetFillColorWithColor(c, color);
    CGContextFillPath(c);
    CGColorRelease(color);
}

// Fills `path` with a gradient between two points.
static void gradient(CGContextRef c, CGPathRef path, int from, int to, CGPoint p0, CGPoint p1) {
    CGColorRef colors[2] = {rgb(from, 1), rgb(to, 1)};
    CFArrayRef array = CFArrayCreate(NULL, (const void **)colors, 2, &kCFTypeArrayCallBacks);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef g = CGGradientCreateWithColors(space, array, NULL);
    CGContextSaveGState(c);
    CGContextAddPath(c, path);
    CGContextClip(c);
    CGContextDrawLinearGradient(c, g, p0, p1, kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(c);
    CGGradientRelease(g);
    CGColorSpaceRelease(space);
    CFRelease(array);
    CGColorRelease(colors[0]);
    CGColorRelease(colors[1]);
}

// Fills `path` with a radial gradient from `inner` at `center` to `outer`
// at `radius`.
static void radial(CGContextRef c, CGPathRef path, int inner, int outer, CGPoint center, CGFloat radius) {
    CGColorRef colors[2] = {rgb(inner, 1), rgb(outer, 1)};
    CFArrayRef array = CFArrayCreate(NULL, (const void **)colors, 2, &kCFTypeArrayCallBacks);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef g = CGGradientCreateWithColors(space, array, NULL);
    CGContextSaveGState(c);
    CGContextAddPath(c, path);
    CGContextClip(c);
    CGContextDrawRadialGradient(c, g, center, 0, center, radius, kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(c);
    CGGradientRelease(g);
    CGColorSpaceRelease(space);
    CFRelease(array);
    CGColorRelease(colors[0]);
    CGColorRelease(colors[1]);
}

static void draw(CGContextRef c) {
    // The rounded square every macOS icon sits in.
    CGPathRef tile = CGPathCreateWithRoundedRect(CGRectMake(100, 100, 824, 824), 185, 185, NULL);
    gradient(c, tile, 0xB4E5FF, 0x5AAEEC, CGPointMake(0, 924), CGPointMake(0, 100));

    // An arcade stick: a ball on a metal shaft in a round base.
    CGContextSaveGState(c);
    CGColorRef soft = rgb(0x000000, 0.3);
    CGContextSetShadowWithColor(c, CGSizeMake(0, -14), 28, soft);
    CGContextBeginTransparencyLayer(c, NULL);

    CGPathRef base = CGPathCreateWithEllipseInRect(CGRectMake(302, 214, 420, 130), NULL);
    fill(c, base, 0x141414);
    CGPathRef rim = CGPathCreateWithEllipseInRect(CGRectMake(332, 244, 360, 92), NULL);
    gradient(c, rim, 0x3A3A3A, 0x1C1C1C, CGPointMake(0, 336), CGPointMake(0, 244));
    CGPathRef shaft = CGPathCreateWithRect(CGRectMake(492, 286, 40, 260), NULL);
    gradient(c, shaft, 0x6A6A6A, 0xE8E8E8, CGPointMake(492, 0), CGPointMake(506, 0));
    CGPathRef collar = CGPathCreateWithEllipseInRect(CGRectMake(476, 272, 72, 28), NULL);
    fill(c, collar, 0x0C0C0C);
    CGPathRef ball = CGPathCreateWithEllipseInRect(CGRectMake(357, 500, 310, 310), NULL);
    radial(c, ball, 0x6A6A6A, 0x060606, CGPointMake(452, 720), 300);

    CGContextEndTransparencyLayer(c);
    CGContextRestoreGState(c);
    CGColorRelease(soft);

    // The ball's shine.
    CGPathRef shine = CGPathCreateWithEllipseInRect(CGRectMake(412, 700, 96, 62), NULL);
    CGColorRef white = rgb(0xFFFFFF, 0.55);
    CGContextAddPath(c, shine);
    CGContextSetFillColorWithColor(c, white);
    CGContextFillPath(c);
    CGColorRelease(white);

    CGPathRef paths[] = {base, rim, shaft, collar, ball, shine, tile};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) CGPathRelease(paths[i]);
}

static void write_png(NSString *dir, NSString *name, int pixels) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, pixels, pixels, 8, 0, space, kCGImageAlphaPremultipliedLast);
    CGContextScaleCTM(c, pixels / 1024.0, pixels / 1024.0);
    draw(c);
    CGImageRef image = CGBitmapContextCreateImage(c);
    NSURL *url = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:name]];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    CGImageDestinationAddImage(dest, image, NULL);
    CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(image);
    CGContextRelease(c);
    CGColorSpaceRelease(space);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: makeicon DIR.iconset\n");
            return 2;
        }
        NSString *dir = @(argv[1]);
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        for (int size = 16; size <= 512; size *= 2) {
            write_png(dir, [NSString stringWithFormat:@"icon_%dx%d.png", size, size], size);
            write_png(dir, [NSString stringWithFormat:@"icon_%dx%d@2x.png", size, size], size * 2);
        }
    }
    return 0;
}
