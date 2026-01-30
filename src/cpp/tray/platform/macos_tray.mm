#ifdef __APPLE__

#include "lemon_tray/platform/macos_tray.h"
#include <iostream>
#include <memory>
#include <string>

// macOS imports for system tray
#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

// -----------------------------------------------------------------------------
// Objective-C Interface
// -----------------------------------------------------------------------------

// Wrapper to hold C++ std::function safely in Obj-C
@interface CallbackWrapper : NSObject {
    std::function<void()> _callback;
}
- (instancetype)initWithCallback:(std::function<void()>)cb;
- (void)execute;
@end

@implementation CallbackWrapper
- (instancetype)initWithCallback:(std::function<void()>)cb {
    self = [super init];
    if (self) { _callback = cb; }
    return self;
}
- (void)execute {
    if (_callback) _callback();
}
@end

// Helper to handle menu actions
@interface MenuActionReceiver : NSObject
- (void)actionHandler:(id)sender;
@end

@implementation MenuActionReceiver
- (void)actionHandler:(id)sender {
    if ([sender isKindOfClass:[NSMenuItem class]]) {
        NSMenuItem *item = (NSMenuItem*)sender;
        if ([item.representedObject isKindOfClass:[CallbackWrapper class]]) {
            [(CallbackWrapper*)item.representedObject execute];
        }
    }
}
@end

// The main tray implementation
// We implement NSUserNotificationCenterDelegate to support the legacy fallback
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
@interface MacOSTrayImpl : NSObject <NSUserNotificationCenterDelegate>
#pragma clang diagnostic pop
@property (strong, nonatomic) NSStatusItem *statusItem;
@property (strong, nonatomic) NSMenu *menu;
@property (strong, nonatomic) CallbackWrapper *readyCallback;
@property (strong, nonatomic) MenuActionReceiver *actionReceiver;
@end

@implementation MacOSTrayImpl
- (instancetype)init {
    self = [super init];
    if (self) {
        _actionReceiver = [[MenuActionReceiver alloc] init];
    }
    return self;
}

- (void)terminateApp:(id)sender {
    if (self.statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:self.statusItem];
        self.statusItem = nil;
    }

    // 2. Notify any observers we're shutting down
    [[NSNotificationCenter defaultCenter] postNotificationName:@"AppWillTerminate" object:nil];

    // 3. Give cleanup handlers a moment to run
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];

    // 4. Force flush any pending I/O
    fflush(stdout);
    fflush(stderr);

    exit (0);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (BOOL)userNotificationCenter:(NSUserNotificationCenter *)center
     shouldPresentNotification:(NSUserNotification *)notification {
    return YES;
}
#pragma clang diagnostic pop

- (void)applicationWillTerminate:(NSNotification *)notification {
    // Clean up the status item when the application is terminating
    if (self.statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:self.statusItem];
        self.statusItem = nil;
    }
}

// Helper method to recursively build NSMenuItem from MenuItem
- (NSMenuItem *)buildMenuItem:(const lemon_tray::MenuItem&)item withImpl:(MacOSTrayImpl*)trayImpl {
    if (item.is_separator) {
        return [NSMenuItem separatorItem];
    }

    NSString *title = [NSString stringWithUTF8String:item.text.c_str()];

    NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title
                                                     action:nil
                                              keyEquivalent:@""];

    // Set properties
    menuItem.enabled = item.enabled;
    if (item.checked) {
        menuItem.state = NSControlStateValueOn;
    }

    // Handle submenu
    if (item.submenu) {
        NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];
        for (const auto& subItem : item.submenu->items) {
            NSMenuItem *subNsMenuItem = [self buildMenuItem:subItem withImpl:trayImpl];
            if (subNsMenuItem) {
                [submenu addItem:subNsMenuItem];
            }
        }
        menuItem.submenu = submenu;
    }
    // Handle callback (only for items without submenu)
    else if (item.callback) {
        CallbackWrapper *wrapper = [[CallbackWrapper alloc] initWithCallback:item.callback];
        menuItem.representedObject = wrapper;
        menuItem.target = trayImpl.actionReceiver;
        menuItem.action = @selector(actionHandler:);
    }

    return menuItem;
}
@end

// -----------------------------------------------------------------------------
// C++ Implementation
// -----------------------------------------------------------------------------

namespace lemon_tray {

MacOSTray::MacOSTray() : impl_(nullptr), tooltip_("") {}

MacOSTray::~MacOSTray() {
    if (impl_) {
        CFRelease(impl_); 
        impl_ = nullptr;
    }
}

bool MacOSTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    MacOSTrayImpl* trayImpl = [[MacOSTrayImpl alloc] init];
    
    if (ready_callback_) {
        trayImpl.readyCallback = [[CallbackWrapper alloc] initWithCallback:ready_callback_];
    }
    
    // ARC Retain
    impl_ = (__bridge_retained void*)trayImpl; 

    NSStatusBar *statusBar = [NSStatusBar systemStatusBar];
    trayImpl.statusItem = [statusBar statusItemWithLength:NSVariableStatusItemLength];

    if (trayImpl.statusItem) {

        if (!icon_path.empty()) {
            set_icon(icon_path);
        }

        trayImpl.menu = [[NSMenu alloc] initWithTitle:[NSString stringWithUTF8String:app_name.c_str()]];

        NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(terminateApp:) // Call our custom method
                                               keyEquivalent:@"q"];
        [quitItem setTarget:trayImpl]; // Target the impl

        [trayImpl.menu addItem:quitItem];

        trayImpl.statusItem.menu = trayImpl.menu;

        // Register for termination notification to clean up status item
        [[NSNotificationCenter defaultCenter] addObserver:trayImpl
                                                 selector:@selector(applicationWillTerminate:)
                                                     name:NSApplicationWillTerminateNotification
                                                   object:nil];

        if (trayImpl.readyCallback) {
            [trayImpl.readyCallback execute];
        }

        return true;
    }

    return false;
}

void MacOSTray::run() {
    if (log_level_ == "debug") {
        std::cout << "[macOS Tray] Entering Run Loop..." << std::endl;
        
    }
    [NSApp run];
}

void MacOSTray::stop() {
    if (impl_) {
        dispatch_async(dispatch_get_main_queue(), ^{
            MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;
            if (trayImpl.statusItem) {
                [[NSStatusBar systemStatusBar] removeStatusItem:trayImpl.statusItem];
                trayImpl.statusItem = nil;
            }
            // For accessory applications, [NSApp terminate:nil] doesn't work properly
            // Force exit the process instead
            exit(0);
        });
    }
}

void MacOSTray::set_menu(const Menu& menu) {
    if (!impl_) return;

    MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;

    // Create a copy of menu items for thread safety
    std::vector<MenuItem> menuItems = menu.items;

    dispatch_async(dispatch_get_main_queue(), ^{
        [trayImpl.menu removeAllItems];

        for (const auto& item : menuItems) {
            NSMenuItem *nsMenuItem = [trayImpl buildMenuItem:item withImpl:trayImpl];
            if (nsMenuItem) {
                [trayImpl.menu addItem:nsMenuItem];
            }
        }
    });
}

void MacOSTray::update_menu() {
    // No-op
}

void MacOSTray::show_notification(const std::string& title, const std::string& message, NotificationType type) {
    NSString *nsTitle = [NSString stringWithUTF8String:title.c_str()];
    NSString *nsMessage = [NSString stringWithUTF8String:message.c_str()];
    std::string current_log_level = log_level_;

    // 1. Production Mode (Bundled App)
    if ([[NSBundle mainBundle] bundleIdentifier] != nil) {
        if (@available(macOS 10.14, *)) {
            UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                  completionHandler:^(BOOL granted, NSError * _Nullable error) {
                if (granted) {
                    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
                    content.title = nsTitle;
                    content.body = nsMessage;
                    
                    NSString *uuid = [[NSUUID UUID] UUIDString];
                    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:uuid content:content trigger:nil];
                    [center addNotificationRequest:request withCompletionHandler:nil];
                }
            }];
        }
    } 
    // 2. CLI/Debug Mode (No Bundle ID) - Uses Deprecated API safely
    else {
        if (current_log_level == "debug") std::cout << "[macOS Tray] Using legacy notification fallback" << std::endl;
        
        // Silence warnings for this specific block
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        
        NSUserNotification *notification = [[NSUserNotification alloc] init];
        notification.title = nsTitle;
        notification.informativeText = nsMessage;
        notification.soundName = NSUserNotificationDefaultSoundName;
        
        MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;
        [[NSUserNotificationCenter defaultUserNotificationCenter] setDelegate:trayImpl];
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        
        #pragma clang diagnostic pop
    }
}

void MacOSTray::set_icon(const std::string& icon_path) {
    if (!impl_ || icon_path.empty()) return;

    icon_path_ = icon_path;
    MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSString *nsPath = [NSString stringWithUTF8String:icon_path.c_str()];
        NSImage *image = [[NSImage alloc] initWithContentsOfFile:nsPath];
        if (image) {
            // Request bar thickness in order to get rid of hard coded icon sizes.
            CGFloat barThickness = [[NSStatusBar systemStatusBar] thickness];
            // Standard padding is usually ~2pts top/bottom, so preferred height is thickness - 4
            CGFloat iconDimension = barThickness - 4.0;
            [image setSize:NSMakeSize(iconDimension, iconDimension)];

            [image setTemplate:NO];  // Allow the icon to display in color
            trayImpl.statusItem.button.image = image;
        }
        else {
            //If the image is unavailable make the title a lemon so it looks like the logo.
            trayImpl.statusItem.button.title = @"🍋";
        }
    });
}

void MacOSTray::set_tooltip(const std::string& tooltip) {
    if (!impl_) return;

    tooltip_ = tooltip;
    MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSString *nsTooltip = [NSString stringWithUTF8String:tooltip.c_str()];
        trayImpl.statusItem.button.toolTip = nsTooltip;
    });
}

void MacOSTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

void MacOSTray::set_log_level(const std::string& log_level) {
    log_level_ = log_level;
}

} // namespace lemon_tray

#endif // __APPLE__
