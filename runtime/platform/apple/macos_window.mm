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

// Dynamically refresh items and checkmarks every time the menu is opened
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

            // Add a temporary placeholder item so macOS knows it has a submenu
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
    void ConfigureWindow(SDL_Window*, bool) {}
    bool RestoreAfterFullscreen(SDL_Window*) { return false; }
    void ShowWindowButtons(SDL_Window*, bool) {}
    float TitleStripHeight(SDL_Window*) { return 0.0f; }
    bool DisplayRefreshFixed(SDL_Window*, double*, double*) { return false; }
    void ForgetWindow(SDL_Window*) {}
}
#endif