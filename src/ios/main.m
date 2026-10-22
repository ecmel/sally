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

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
    _game = [GameViewController new];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = _game;
    [self.window makeKeyAndVisible];
    // A cartridge opened from the Files app that launched Sally.
    NSURL *url = options.URLContexts.anyObject.URL;
    if (url) [_game openURL:url];
}

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)contexts {
    NSURL *url = contexts.anyObject.URL;
    if (url) [_game openURL:url];
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
