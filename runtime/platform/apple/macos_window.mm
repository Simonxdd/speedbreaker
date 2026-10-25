// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ../macos_window.h.
#import <TargetConditionals.h>
#import <AppKit/AppKit.h>
#include <cmath>
#include <SDL3/SDL.h>

#include <platform/macos_window.h>

namespace ui
{
    void OpenSettings();
    void CloseSettings();
    bool IsSettingsOpen();
}

struct MenuEnumEntry {
    double value;
    char label[64];
    bool selected;
};

struct MenuOptionInfo {
    int id;
    const char* label;
    int type;
    char currentValue[64];
    int entryCount;
    MenuEnumEntry entries[16];
};

extern "C" {
    int UiGetCategoryCount();
    const char* UiGetCategoryName(int catIndex);
    void UiToggleOption(int optionId);
    void UiSetOptionValue(int optionId, double value);
    bool UiGetOptionState(int optionId);
    int UiGetOptionsForCategory(int catIndex, MenuOptionInfo* outBuffer, int maxCount);
}

@interface SpeedBreakerMenuDelegate : NSObject <NSMenuDelegate>
@property (nonatomic, assign) int categoryIndex;
@end

@implementation SpeedBreakerMenuDelegate
- (void)handleMenuAction:(NSMenuItem *)sender {
    NSInteger tag = sender.tag;
    if (tag == 999) {
        if (ui::IsSettingsOpen()) {
            ui::CloseSettings();
        } else {
            ui::OpenSettings();
        }
    } else {
        UiToggleOption((int)tag);
    }
}

- (void)handleEnumChoiceAction:(NSMenuItem *)sender {
    NSDictionary *info = sender.representedObject;
    if (info) {
        int optId = [info[@"optId"] intValue];
        double val = [info[@"value"] doubleValue];
        UiSetOptionValue(optId, val);
    }
}

- (void)menuWillOpen:(NSMenu *)menu {
    if (self.categoryIndex < 0)
        return;

    [menu removeAllItems];

    MenuOptionInfo optionsBuf[64];
    int optCount = UiGetOptionsForCategory(self.categoryIndex, optionsBuf, 64);

    if (optCount > 0) {
        for (int j = 0; j < optCount; j++) {
            NSString *optLabel = [NSString stringWithUTF8String:optionsBuf[j].label];
            NSString *valStr = [NSString stringWithUTF8String:optionsBuf[j].currentValue];

            NSMenuItem *optItem = [[NSMenuItem alloc] init];
            optItem.target = self;

            if (optionsBuf[j].type == 0) { // Type::Bool
                optItem.title = optLabel;
                optItem.action = @selector(handleMenuAction:);
                optItem.tag = optionsBuf[j].id;
                optItem.state = UiGetOptionState(optionsBuf[j].id) ? NSControlStateValueOn : NSControlStateValueOff;
            } else if (optionsBuf[j].entryCount > 0) { // Type::Enum / Choices
                optItem.title = [NSString stringWithFormat:@"%@: %@", optLabel, valStr];
                NSMenu *enumSub = [[NSMenu alloc] initWithTitle:optLabel];
                for (int e = 0; e < optionsBuf[j].entryCount; e++) {
                    NSString *eLabel = [NSString stringWithUTF8String:optionsBuf[j].entries[e].label];
                    NSMenuItem *eItem = [[NSMenuItem alloc] initWithTitle:eLabel
                                                                  action:@selector(handleEnumChoiceAction:)
                                                           keyEquivalent:@""];
                    eItem.target = self;
                    eItem.state = optionsBuf[j].entries[e].selected ? NSControlStateValueOn : NSControlStateValueOff;
                    eItem.representedObject = @{
                        @"optId": @(optionsBuf[j].id),
                        @"value": @(optionsBuf[j].entries[e].value)
                    };
                    [enumSub addItem:eItem];
                    [eItem release];
                }
                [optItem setSubmenu:enumSub];
                [enumSub release];
            } else { // Sliders / Numeric
                optItem.title = [NSString stringWithFormat:@"%@: %@", optLabel, valStr];
                optItem.action = @selector(handleMenuAction:);
                optItem.tag = optionsBuf[j].id;
            }

            [menu addItem:optItem];
            [optItem release];
        }
    } else {
        NSMenuItem *emptyItem = [[NSMenuItem alloc] initWithTitle:@"(No options)" action:nil keyEquivalent:@""];
        [menu addItem:emptyItem];
        [emptyItem release];
    }
}
@end

extern "C" void SetupMacMenuBar()
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        NSMenu *mainMenu = app.mainMenu;
        if (!mainMenu || [mainMenu itemWithTitle:@"Display"])
            return;

        NSInteger windowIndex = [mainMenu indexOfItemWithTitle:@"Window"];
        if (windowIndex == -1)
            windowIndex = mainMenu.numberOfItems;

        // Overlay Toggle Menu
        SpeedBreakerMenuDelegate *overlayDelegate = [[SpeedBreakerMenuDelegate alloc] init];
        overlayDelegate.categoryIndex = -1;

        NSMenuItem *overlayRoot = [[NSMenuItem alloc] initWithTitle:@"Overlay" action:nil keyEquivalent:@""];
        NSMenu *overlaySub = [[NSMenu alloc] initWithTitle:@"Overlay"];
        NSMenuItem *toggleItem = [[NSMenuItem alloc] initWithTitle:@"Toggle In-Game Settings (F1)"
                                                            action:@selector(handleMenuAction:)
                                                     keyEquivalent:@"1"];
        toggleItem.target = overlayDelegate;
        toggleItem.tag = 999;
        [overlaySub addItem:toggleItem];
        [toggleItem release];
        [overlayRoot setSubmenu:overlaySub];
        [overlaySub release];
        [mainMenu insertItem:overlayRoot atIndex:windowIndex++];
        [overlayRoot release];

        // Categories (Display, Graphics, Audio, etc.)
        int catCount = UiGetCategoryCount();
        for (int i = 0; i < catCount; i++) {
            const char* namePtr = UiGetCategoryName(i);
            if (!namePtr) continue;

            NSString *catName = [NSString stringWithUTF8String:namePtr];
            NSMenuItem *catRoot = [[NSMenuItem alloc] initWithTitle:catName action:nil keyEquivalent:@""];

            NSMenu *catSub = [[NSMenu alloc] initWithTitle:catName];
            SpeedBreakerMenuDelegate *catDelegate = [[SpeedBreakerMenuDelegate alloc] init];
            catDelegate.categoryIndex = i;
            catSub.delegate = catDelegate;

            NSMenuItem *placeholder = [[NSMenuItem alloc] initWithTitle:@"Loading..." action:nil keyEquivalent:@""];
            [catSub addItem:placeholder];
            [placeholder release];

            [catRoot setSubmenu:catSub];
            [catSub release];
            [mainMenu insertItem:catRoot atIndex:windowIndex++];
            [catRoot release];
        }
    }
}

#if TARGET_OS_OSX
namespace platform::macos
{
    namespace
    {
        NSWindow* s_window = nil;
        bool s_inTransition = false;
        NSMutableArray* s_observers = nil;

        NSWindow* CocoaWindow(SDL_Window* window)
        {
            if (!window)
                return nil;
            return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                nullptr);
        }

        void ApplyLook(NSWindow* window)
        {
            window.titleVisibility = NSWindowTitleHidden;
            window.titlebarAppearsTransparent = YES;
            window.styleMask |= NSWindowStyleMaskFullSizeContentView;
        }

        void Observe(NSWindow* window, NSString* name, void (^block)(NSNotification*))
        {
            id token = [NSNotificationCenter.defaultCenter addObserverForName:name object:window queue:nil usingBlock:block];
            [s_observers addObject:token];
        }
    }

    void ConfigureWindow(SDL_Window* window, bool windowed)
    {
        NSWindow* w = CocoaWindow(window);
        if (!w)
            return;

        if (s_window != w)
        {
            ForgetWindow(nullptr);
            s_window = w;
            s_observers = [[NSMutableArray alloc] init];
            for (NSString* name in @[ NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification ])
                Observe(w, name, ^(NSNotification*) { s_inTransition = true; });
            for (NSString* name in @[ NSWindowDidEnterFullScreenNotification, NSWindowDidExitFullScreenNotification,
                     @"NSWindowDidFailToEnterFullScreenNotification", @"NSWindowDidFailToExitFullScreenNotification" ])
                Observe(w, name, ^(NSNotification*) { s_inTransition = false; });
            Observe(w, NSWindowWillEnterFullScreenNotification, ^(NSNotification* n) {
                NSWindow* nw = n.object;
                if (nw.styleMask & NSWindowStyleMaskFullSizeContentView)
                    nw.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
            });
        }
        if (windowed)
            ApplyLook(w);
    }

    bool RestoreAfterFullscreen(SDL_Window* window)
    {
        NSWindow* w = CocoaWindow(window);
        if (!w || s_inTransition || (w.styleMask & NSWindowStyleMaskFullScreen))
            return false;
        const NSRect frame = w.frame;
        ApplyLook(w);
        [w setFrame:frame display:YES];
        return true;
    }

    void ShowWindowButtons(SDL_Window* window, bool shown)
    {
        NSWindow* w = CocoaWindow(window);
        if (!w)
            return;
        const NSWindowButton buttons[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
        for (NSWindowButton b : buttons)
        {
            NSButton* button = [w standardWindowButton:b];
            if (button && button.hidden == shown)
                button.hidden = !shown;
        }
    }

    float TitleStripHeight(SDL_Window* window)
    {
        NSWindow* w = CocoaWindow(window);
        if (!w || (w.styleMask & NSWindowStyleMaskFullScreen) || !(w.styleMask & NSWindowStyleMaskFullSizeContentView))
            return 0.0f;
        const CGFloat strip = NSHeight(w.frame) - NSMaxY(w.contentLayoutRect);
        return strip > 0 ? float(strip) : 0.0f;
    }

    bool DisplayRefreshFixed(SDL_Window* window, double* minHz, double* maxHz)
    {
        NSWindow* w = CocoaWindow(window);
        NSScreen* screen = w ? w.screen : nil;
        if (!screen)
            return false;
        const NSTimeInterval shortest = screen.minimumRefreshInterval, longest = screen.maximumRefreshInterval;
        if (minHz)
            *minHz = longest > 0 ? 1.0 / longest : 0.0;
        if (maxHz)
            *maxHz = shortest > 0 ? 1.0 / shortest : 0.0;
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number && CGDisplayIsBuiltin(CGDirectDisplayID(number.unsignedIntValue)))
            return false;
        return shortest > 0 && longest > 0 && std::abs(longest - shortest) <= 0.01 * shortest;
    }

    void ForgetWindow(SDL_Window*)
    {
        for (id token in s_observers)
            [NSNotificationCenter.defaultCenter removeObserver:token];
        [s_observers release];
        s_observers = nil;
        s_window = nil;
        s_inTransition = false;
    }
}
#endif