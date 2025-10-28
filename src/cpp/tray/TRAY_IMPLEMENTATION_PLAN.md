# C++ Tray Application Implementation Plan

## Architecture Overview

### Clean Separation Design

```
┌─────────────────────────────────────┐
│  lemonade-server-beta[.exe]         │  <- Tray Application
│  - System tray UI                   │
│  - Process lifecycle management     │
│  - User interaction handling        │
└─────────────┬───────────────────────┘
              │ spawns & manages
              ↓
┌─────────────────────────────────────┐
│  lemonade[.exe]                     │  <- Server Binary
│  - HTTP server                      │
│  - Model management                 │
│  - API endpoints                    │
└─────────────────────────────────────┘
```

**Key Architectural Improvements:**
1. **Separate Binaries**: `lemonade-server-beta` (tray) and `lemonade` (server)
2. **Single Responsibility**: Tray manages UI/lifecycle, server handles LLM operations
3. **Clean IPC**: Communication via HTTP API (localhost) and process signals
4. **No Circular Dependencies**: Tray spawns server, not vice versa

## Project Structure

```
src/cpp/
├── tray/                           # New directory for tray app
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── include/
│   │   └── lemon_tray/
│   │       ├── tray_app.h          # Main tray application class
│   │       ├── server_manager.h    # Manages lemonade-router.exe process
│   │       ├── menu_builder.h      # Builds context menu
│   │       ├── version_checker.h   # Background version checking
│   │       ├── model_monitor.h     # Monitors loaded models
│   │       └── platform/
│   │           ├── tray_interface.h    # Abstract tray interface
│   │           ├── windows_tray.h      # Windows implementation
│   │           ├── macos_tray.h        # macOS implementation
│   │           └── linux_tray.h        # Ubuntu/Linux implementation
│   └── src/
│       ├── tray_app.cpp
│       ├── server_manager.cpp
│       ├── menu_builder.cpp
│       ├── version_checker.cpp
│       ├── model_monitor.cpp
│       └── platform/
│           ├── windows_tray.cpp
│           ├── macos_tray.cpp
│           └── linux_tray.cpp
└── [existing server code...]
```

## Feature Parity with Python Implementation

### Core Features (from tray.py)

1. **System Tray Icon**
   - Platform-native icon display
   - Context menu on right-click (Windows/Linux) or left-click (macOS)
   - Tooltip showing server status

2. **Menu Items**
   - **Status Display**: Show currently loaded model or "No models loaded"
   - **Load Model Submenu**: List all downloaded models with checkmark on loaded
   - **Unload LLM**: Unload currently loaded model
   - **Port Selection**: Submenu with ports (8000, 8020, 8040, 8060, 8080, 9000)
   - **Context Size**: Submenu with options (4K, 8K, 16K, 32K, 64K, 128K)
   - **Documentation**: Open browser to https://lemonade-server.ai/docs/
   - **LLM Chat**: Open browser to http://localhost:{port}/#llm-chat
   - **Model Manager**: Open browser to http://localhost:{port}/#model-management
   - **Logs Submenu**:
     - Show Logs: Open log viewer
     - Enable Debug Logs: Toggle debug mode (with checkmark)
   - **Upgrade**: Show when new version available, download installer
   - **Quit Lemonade**: Stop server and exit tray

3. **Background Operations**
   - Model list updates (every 1s initially, then 10s)
   - Version checking (every 15 minutes)
   - Server health monitoring

4. **Notifications**
   - Startup notification
   - Port/context size change notifications
   - Upgrade available notifications
   - Error notifications

5. **Log Management**
   - Write logs to file
   - Open logs in terminal/viewer
   - Support debug mode toggle

## Platform-Specific Implementation

### Windows Implementation

**Technology Stack:**
- Win32 API for system tray (Shell_NotifyIcon)
- Win32 menus with checkmarks and submenus
- Balloon notifications via Shell_NotifyIcon
- Process management via CreateProcess/TerminateProcess
- PowerShell for log viewing

**Key APIs:**
```cpp
// System tray
Shell_NotifyIcon(NIM_ADD, &nid);
Shell_NotifyIcon(NIM_MODIFY, &nid);  // For notifications

// Menu creation
CreatePopupMenu();
AppendMenu();
TrackPopupMenu();

// Process management
CreateProcess();
WaitForSingleObject();
TerminateProcess();
```

**Dependencies:** None (all standard Windows SDK)

### macOS Implementation

**Technology Stack:**
- Objective-C++ for NSStatusBar integration
- NSMenu for menu management
- NSUserNotification for notifications
- POSIX process management (fork/exec)
- Terminal.app for log viewing

**Key Classes:**
```objc
// System tray
NSStatusBar *statusBar = [NSStatusBar systemStatusBar];
NSStatusItem *statusItem = [statusBar statusItemWithLength:NSVariableStatusItemLength];

// Menu
NSMenu *menu = [[NSMenu alloc] init];
[menu addItem:item];

// Notifications
NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
```

**Dependencies:** Cocoa, Foundation (system frameworks)

### Linux (Ubuntu) Implementation

**Technology Stack:**
- **libappindicator3** for system tray (Ubuntu/GNOME standard)
- GTK+ 3 for menu management
- libnotify for desktop notifications
- POSIX process management
- xterm/gnome-terminal for log viewing

**Key APIs:**
```cpp
// System tray (via libappindicator)
AppIndicator *indicator = app_indicator_new(
    "lemonade-server",
    "icon-path",
    APP_INDICATOR_CATEGORY_APPLICATION_STATUS
);

// Menu (GTK)
GtkWidget *menu = gtk_menu_new();
gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

// Notifications
notify_init("Lemonade Server");
NotifyNotification *n = notify_notification_new(title, message, icon);
notify_notification_show(n, NULL);
```

**Dependencies:**
- libappindicator3-dev
- libgtk-3-dev
- libnotify-dev
- pkg-config

## Component Design

### 1. TrayInterface (Abstract Base Class)

```cpp
class TrayInterface {
public:
    virtual ~TrayInterface() = default;
    
    // Lifecycle
    virtual bool initialize(const std::string& icon_path) = 0;
    virtual void run() = 0;
    virtual void stop() = 0;
    
    // Menu management
    virtual void set_menu(const Menu& menu) = 0;
    virtual void update_menu() = 0;
    
    // Notifications
    virtual void show_notification(
        const std::string& title,
        const std::string& message,
        NotificationType type = NotificationType::INFO
    ) = 0;
    
    // Icon management
    virtual void set_icon(const std::string& icon_path) = 0;
    virtual void set_tooltip(const std::string& tooltip) = 0;
};
```

### 2. ServerManager

```cpp
class ServerManager {
public:
    // Server lifecycle
    bool start_server(
        const std::string& server_binary_path,
        int port,
        int ctx_size,
        const std::string& log_file
    );
    bool stop_server();
    bool restart_server();
    bool is_server_running();
    
    // Configuration
    void set_port(int port);
    void set_context_size(int ctx_size);
    void set_log_level(LogLevel level);
    
    // API communication
    nlohmann::json get_health();
    nlohmann::json get_models();
    bool load_model(const std::string& model_name);
    bool unload_model();
    
private:
    pid_t server_pid_;
    std::string server_binary_path_;
    int port_;
    int ctx_size_;
    std::string log_file_;
    
    // HTTP client for API calls
    std::unique_ptr<HttpClient> http_client_;
};
```

### 3. MenuBuilder

```cpp
class MenuBuilder {
public:
    Menu build_menu(
        const ServerState& state,
        const std::vector<ModelInfo>& models,
        const VersionInfo& version
    );
    
private:
    Menu build_model_submenu(const std::vector<ModelInfo>& models);
    Menu build_port_submenu(int current_port);
    Menu build_context_size_submenu(int current_ctx_size);
    Menu build_logs_submenu(bool debug_enabled);
};
```

### 4. ModelMonitor

```cpp
class ModelMonitor {
public:
    ModelMonitor(const std::string& base_url);
    
    // Start background monitoring
    void start(std::chrono::seconds initial_interval,
               std::chrono::seconds regular_interval);
    void stop();
    
    // Get current state
    std::vector<ModelInfo> get_downloaded_models() const;
    std::optional<std::string> get_loaded_model() const;
    
    // Callbacks
    void on_models_changed(std::function<void()> callback);
    
private:
    void monitor_loop();
    
    std::thread monitor_thread_;
    std::atomic<bool> should_stop_;
    mutable std::mutex models_mutex_;
    std::vector<ModelInfo> downloaded_models_;
    std::optional<std::string> loaded_model_;
};
```

### 5. VersionChecker

```cpp
class VersionChecker {
public:
    VersionChecker(const std::string& current_version);
    
    void start(std::chrono::minutes check_interval);
    void stop();
    
    bool is_update_available() const;
    std::string get_latest_version() const;
    std::string get_download_url() const;
    
private:
    void check_loop();
    void check_github_release();
    
    std::string current_version_;
    std::string latest_version_;
    std::string download_url_;
    std::thread check_thread_;
    std::atomic<bool> should_stop_;
};
```

### 6. TrayApp (Main Application Class)

```cpp
class TrayApp {
public:
    TrayApp(int argc, char* argv[]);
    ~TrayApp();
    
    int run();
    
private:
    // Initialization
    void parse_arguments();
    void setup_logging();
    void find_server_binary();
    
    // Menu actions
    void on_load_model(const std::string& model_name);
    void on_unload_model();
    void on_change_port(int new_port);
    void on_change_context_size(int new_ctx_size);
    void on_toggle_debug_logs();
    void on_show_logs();
    void on_open_documentation();
    void on_open_llm_chat();
    void on_open_model_manager();
    void on_upgrade();
    void on_quit();
    
    // Menu building
    void refresh_menu();
    
    // Components
    std::unique_ptr<TrayInterface> tray_;
    std::unique_ptr<ServerManager> server_manager_;
    std::unique_ptr<ModelMonitor> model_monitor_;
    std::unique_ptr<VersionChecker> version_checker_;
    MenuBuilder menu_builder_;
    
    // Configuration
    std::string server_binary_path_;
    std::string log_file_path_;
    int port_;
    int ctx_size_;
    bool debug_logs_enabled_;
};
```

## Command-Line Interface

### lemonade-server-beta Usage

```bash
# Launch tray app (which starts server)
lemonade-server-beta [options]

Options:
  --port PORT              Server port (default: 8000)
  --ctx-size SIZE          Context size (default: 4096)
  --log-file PATH          Log file path
  --log-level LEVEL        Log level: debug, info, warning, error
  --server-binary PATH     Path to lemonade server binary
  --no-tray               Start server without tray (headless mode)
  --help                  Show help message
  --version               Show version

Examples:
  # Start with tray
  lemonade-server-beta

  # Start on custom port
  lemonade-server-beta --port 8080

  # Start without tray (headless)
  lemonade-server-beta --no-tray

  # Custom server binary location
  lemonade-server-beta --server-binary /opt/lemonade/lemonade
```

### --no-tray Implementation

When `--no-tray` is specified:
1. Skip tray initialization
2. Start `lemonade` server directly in foreground
3. Forward server stdout/stderr to console
4. Handle Ctrl+C to gracefully stop server
5. Exit when server exits

## Implementation Phases

### Phase 1: Platform Abstraction Layer (1-2 weeks)
- Define `TrayInterface` abstract class
- Implement basic Windows tray with icon and simple menu
- Implement basic macOS tray with icon and simple menu
- Implement basic Linux tray with libappindicator
- Test icon display and basic menu on all platforms

### Phase 2: Server Management (1 week)
- Implement `ServerManager` class
- Process spawning and lifecycle management
- HTTP client for API communication
- Server health monitoring
- Test server start/stop on all platforms

### Phase 3: Menu System (1 week)
- Implement `MenuBuilder` class
- Dynamic menu generation
- Submenu support with checkmarks
- Menu action callbacks
- Test all menu items

### Phase 4: Background Services (1 week)
- Implement `ModelMonitor` class
- Implement `VersionChecker` class
- Background thread management
- Thread-safe state updates
- Test concurrent operations

### Phase 5: Main Application (1 week)
- Implement `TrayApp` class
- Command-line argument parsing
- All menu action handlers
- Configuration persistence
- Test end-to-end workflows

### Phase 6: Platform Polish (1-2 weeks)
- Platform-specific notification styling
- Log viewer integration per platform
- Icon quality and system integration
- Handle edge cases (server crashes, network failures)
- Test on multiple versions of each OS

### Phase 7: Installer Integration (1 week)
- Update Windows NSIS installer
- Create macOS application bundle
- Create Linux .deb package
- Test installation and shortcuts
- Test PATH integration

## Testing Strategy

### Unit Tests
- `ServerManager` process lifecycle
- `MenuBuilder` menu generation
- `ModelMonitor` state updates
- `VersionChecker` version parsing

### Integration Tests
- Full tray → server → API flow
- Menu actions trigger correct API calls
- Server restart preserves state
- Concurrent model load requests

### Platform Tests
- Visual testing on Windows 10/11
- Visual testing on macOS Monterey/Ventura/Sonoma
- Visual testing on Ubuntu 20.04/22.04/24.04
- Test with different desktop environments (GNOME, KDE, XFCE)

### User Acceptance Tests
- Install via installer on clean systems
- Verify shortcuts work
- Verify PATH additions
- Test `--no-tray` mode
- Test upgrade workflow

## Dependencies

### Windows
- Standard Win32 libraries (included with Windows SDK)
- No additional dependencies

### macOS
- Cocoa framework (system)
- Foundation framework (system)
- Xcode command line tools

### Linux
- libappindicator3-dev
- libgtk-3-dev
- libnotify-dev
- pkg-config

Install command:
```bash
sudo apt-get install libappindicator3-dev libgtk-3-dev libnotify-dev pkg-config
```

## Risk Mitigation

### 1. Linux Desktop Environment Fragmentation
- **Risk**: Different DEs handle system trays differently
- **Mitigation**: Use libappindicator3 (standard across GNOME/Unity/KDE)
- **Fallback**: Provide command-line-only mode if tray unavailable

### 2. macOS Sandbox/Permissions
- **Risk**: Notarization and code signing required
- **Mitigation**: Set up proper entitlements, test notarization early
- **Fallback**: Provide unsigned version for development

### 3. Windows Defender False Positives
- **Risk**: Process spawning may trigger AV
- **Mitigation**: Sign binaries, submit to Microsoft for analysis
- **Fallback**: Document allowlist instructions

### 4. Process Management Edge Cases
- **Risk**: Orphaned server processes
- **Mitigation**: Use PID files, cleanup on startup, handle signals properly
- **Fallback**: Provide manual `lemonade stop` command

## Success Criteria

✅ Single `lemonade-server-beta` executable launches successfully on all platforms  
✅ Tray icon appears in system tray with proper icon  
✅ All menu items functional and match Python reference  
✅ Server process lifecycle managed correctly  
✅ `--no-tray` mode works as headless server  
✅ Installer creates shortcuts and adds to PATH  
✅ Memory usage < 20MB for tray app  
✅ No memory leaks during extended operation  
✅ All existing server tests pass with tray app  

---

This plan provides a complete roadmap for implementing a production-ready, cross-platform tray application that improves upon the Python reference implementation's architecture while maintaining feature parity.


