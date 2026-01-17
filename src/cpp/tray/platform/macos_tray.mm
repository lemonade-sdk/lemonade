#ifdef __APPLE__

#include "lemon_tray/platform/macos_tray.h"
#include <iostream>
#include <memory>

// macOS imports for system tray
#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

// Forward declaration of Objective-C implementation
@interface MacOSTrayImpl : NSObject
@property (strong, nonatomic) NSStatusItem *statusItem;
@property (strong, nonatomic) NSMenu *menu;
@property (strong, nonatomic) NSImage *iconImage;
@property (assign, nonatomic) std::function<void()> readyCallback;
@property (assign, nonatomic) std::function<void(const std::string&)> menuCallback;
@end

// Menu item wrapper to store C++ callback
@interface MenuItemWrapper : NSObject
@property (assign, nonatomic) std::function<void()> callback;
@end

namespace lemon_tray {

MacOSTray::MacOSTray()
    : impl_(nullptr)
{
}

MacOSTray::~MacOSTray() {
    if (impl_) {
        [(__bridge MacOSTrayImpl*)impl_ release];
        impl_ = nullptr;
    }
}

bool MacOSTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;

    // Create the Objective-C implementation
    MacOSTrayImpl* trayImpl = [[MacOSTrayImpl alloc] init];
    trayImpl.readyCallback = ready_callback_;
    impl_ = (__bridge void*)trayImpl;

    // Initialize the status bar
    NSStatusBar *statusBar = [NSStatusBar systemStatusBar];
    trayImpl.statusItem = [statusBar statusItemWithLength:NSSquareStatusItemLength];

    // Set up the status item
    if (trayImpl.statusItem) {
        trayImpl.statusItem.button.title = @"🍋"; // Lemon emoji as default icon

        // Load custom icon if provided
        if (!icon_path.empty()) {
            set_icon(icon_path);
        }

        // Create initial menu
        trayImpl.menu = [[NSMenu alloc] initWithTitle:@"Lemonade"];

        // Add default items
        NSMenuItem *showItem = [[NSMenuItem alloc] initWithTitle:@"Show Lemonade"
                                                         action:@selector(showLemonade:)
                                                  keyEquivalent:@""];
        showItem.target = trayImpl;
        [trayImpl.menu addItem:showItem];

        NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                         action:@selector(quitApplication:)
                                                  keyEquivalent:@"q"];
        quitItem.target = trayImpl;
        [trayImpl.menu addItem:quitItem];

        trayImpl.statusItem.menu = trayImpl.menu;

        if (ready_callback_) {
            ready_callback_();
        }

        return true;
    }

    return false;
}

void MacOSTray::run() {
    // On macOS, the main application run loop should already be running
    // This method is here for interface compatibility
    if (log_level_ == "debug") {
        std::cout << "[macOS Tray] Tray is active" << std::endl;
    }
}

void MacOSTray::stop() {
    if (impl_) {
        MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;
        if (trayImpl.statusItem) {
            [[NSStatusBar systemStatusBar] removeStatusItem:trayImpl.statusItem];
            trayImpl.statusItem = nil;
        }
    }
}

void MacOSTray::set_menu(const Menu& menu) {
    if (!impl_) return;

    MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;

    // Clear existing menu
    [trayImpl.menu removeAllItems];

    // Add menu items
    for (const auto& item : menu.items) {
        if (item.is_separator) {
            [trayImpl.menu addItem:[NSMenuItem separatorItem]];
        } else {
            NSString *title = [NSString stringWithUTF8String:item.text.c_str()];
            NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title
                                                             action:@selector(menuItemClicked:)
                                                      keyEquivalent:@""];

            if (item.enabled) {
                menuItem.target = trayImpl;
            } else {
                menuItem.enabled = NO;
            }

            // Handle checkable items
            if (item.checked) {
                menuItem.state = NSControlStateValueOn;
            }

            // Store callback in wrapper
            if (item.callback) {
                MenuItemWrapper *wrapper = [[MenuItemWrapper alloc] init];
                wrapper.callback = item.callback;
                menuItem.representedObject = wrapper;
            }

            [trayImpl.menu addItem:menuItem];
        }
    }
}

void MacOSTray::update_menu() {
    // Menu is automatically updated when set_menu is called
}

void MacOSTray::show_notification(
    const std::string& title,
    const std::string& message,
    NotificationType type)
{
    // Use modern UserNotifications framework (macOS 10.14+)
    NSString *nsTitle = [NSString stringWithUTF8String:title.c_str()];
    NSString *nsMessage = [NSString stringWithUTF8String:message.c_str()];

    // Request notification permissions if not already granted
    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
                          completionHandler:^(BOOL granted, NSError * _Nullable error) {
        if (granted) {
            // Create notification content
            UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
            content.title = nsTitle;
            content.body = nsMessage;
            content.sound = [UNNotificationSound defaultSound];

            // Set notification category based on type
            switch (type) {
                case NotificationType::INFO:
                    // Default category
                    break;
                case NotificationType::WARNING:
                    content.categoryIdentifier = @"warning";
                    break;
                case NotificationType::ERROR:
                    content.categoryIdentifier = @"error";
                    break;
                case NotificationType::SUCCESS:
                    content.categoryIdentifier = @"success";
                    break;
            }

            // Create unique identifier for the notification
            NSString *identifier = [NSString stringWithFormat:@"lemonade-%f", [[NSDate date] timeIntervalSince1970]];

            // Create the request
            UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                  content:content
                                                                                  trigger:nil];

            // Schedule the notification
            [center addNotificationRequest:request withCompletionHandler:^(NSError * _Nullable error) {
                if (error) {
                    if (log_level_ == "debug") {
                        std::cout << "[macOS Tray] Failed to show notification: " << [error.localizedDescription UTF8String] << std::endl;
                    }
                }
            }];
        } else if (log_level_ == "debug") {
            std::cout << "[macOS Tray] Notification permission denied" << std::endl;
        }
    }];
}

void MacOSTray::set_icon(const std::string& icon_path) {
    if (!impl_) return;

    icon_path_ = icon_path;
    MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;

    if (!icon_path.empty()) {
        NSString *nsPath = [NSString stringWithUTF8String:icon_path.c_str()];
        NSImage *image = [[NSImage alloc] initWithContentsOfFile:nsPath];

        if (image) {
            // Resize to status bar size (typically 18x18)
            NSSize iconSize = NSMakeSize(18, 18);
            [image setSize:iconSize];

            trayImpl.statusItem.button.image = image;
            trayImpl.iconImage = image;
        }
    }
}

void MacOSTray::set_tooltip(const std::string& tooltip) {
    if (!impl_) return;

    MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;

    // macOS status items don't have tooltips, but we can set accessibility label
    NSString *nsTooltip = [NSString stringWithUTF8String:tooltip.c_str()];
    trayImpl.statusItem.button.accessibilityLabel = nsTooltip;
}

void MacOSTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
    if (impl_) {
        MacOSTrayImpl* trayImpl = (__bridge MacOSTrayImpl*)impl_;
        trayImpl.readyCallback = callback;
    }
}

void MacOSTray::set_log_level(const std::string& log_level) {
    log_level_ = log_level;
}

} // namespace lemon_tray

// Objective-C implementation
@implementation MacOSTrayImpl

- (instancetype)init {
    self = [super init];
    if (self) {
        self.statusItem = nil;
        self.menu = nil;
        self.iconImage = nil;
    }
    return self;
}

- (void)dealloc {
    if (self.statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:self.statusItem];
    }
    [self.menu release];
    [self.iconImage release];
    [super dealloc];
}

- (void)showLemonade:(id)sender {
    // Bring Lemonade to front - implementation depends on how the app is structured
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
}

- (void)quitApplication:(id)sender {
    [[NSApplication sharedApplication] terminate:self];
}

- (void)menuItemClicked:(id)sender {
    NSMenuItem *menuItem = (NSMenuItem *)sender;
    MenuItemWrapper *wrapper = (MenuItemWrapper *)menuItem.representedObject;

    if (wrapper && wrapper.callback) {
        wrapper.callback();
    }
}

@end

@implementation MenuItemWrapper

- (instancetype)init {
    self = [super init];
    return self;
}

- (void)dealloc {
    [super dealloc];
}

@end

#endif // __APPLE__
