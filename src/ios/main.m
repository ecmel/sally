// Sally for iPhone and iPad: the application and its one scene.

#import <UIKit/UIKit.h>

#import "GameViewController.h"

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SceneDelegate {
    GameViewController *_game;
}

// Sorted by name, so the disks of a set go into D1 onwards in order.
static NSArray<NSURL *> *urls(NSSet<UIOpenURLContext *> *contexts) {
    NSMutableArray<NSURL *> *urls = [NSMutableArray array];
    for (UIOpenURLContext *context in contexts) [urls addObject:context.URL];
    return [urls sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
}

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
    _game = [GameViewController new];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = _game;
    [self.window makeKeyAndVisible];
    // Files opened from the Files app that launched Sally.
    [_game openURLs:urls(options.URLContexts)];
}

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)contexts {
    [_game openURLs:urls(contexts)];
}

- (void)sceneWillResignActive:(UIScene *)scene {
    _game.suspended = YES;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    _game.suspended = NO;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
