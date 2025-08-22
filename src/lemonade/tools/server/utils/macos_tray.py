import os
import sys
import platform
import subprocess
from typing import Dict, Set, Callable, List, Optional, Tuple, Any

# Check if we're on macOS and import accordingly
if platform.system() == "Darwin":
    try:
        import rumps
        RUMPS_AVAILABLE = True
    except ImportError:
        RUMPS_AVAILABLE = False
        print("Warning: rumps not available. Install with: pip install rumps")
else:
    RUMPS_AVAILABLE = False


class MenuItem:
    """Cross-platform menu item representation."""
    
    def __init__(
        self,
        text: str,
        callback: Optional[Callable] = None,
        enabled: bool = True,
        submenu=None,
        checked: bool = False,
    ):
        self.text = text
        self.callback = callback
        self.enabled = enabled
        self.submenu = submenu
        self.checked = checked


class Menu:
    """Cross-platform menu representation."""
    SEPARATOR = "SEPARATOR"

    def __init__(self, *items):
        self.items = list(items)


class MacOSSystemTray:
    """
    macOS-specific system tray implementation using rumps.
    """

    def __init__(self, app_name: str, icon_path: str):
        if not RUMPS_AVAILABLE:
            raise ImportError("rumps library is required for macOS tray support")
            
        self.app_name = app_name
        self.icon_path = icon_path
        self.app = None
        self.menu_callbacks = {}  # Store callbacks by menu item title
        
    def create_menu(self):
        """
        Create the context menu based on current state. Override in subclass.
        """
        return Menu(MenuItem("Exit", self.exit_app))
        
    def build_rumps_menu(self, menu_items):
        """
        Convert our menu structure to rumps menu items.
        """
        rumps_items = []
        
        for item in menu_items:
            if item == Menu.SEPARATOR:
                rumps_items.append(rumps.separator)
            elif isinstance(item, MenuItem):
                if item.submenu:
                    # Create submenu
                    submenu_items = self.build_rumps_menu(item.submenu.items)
                    submenu = rumps.MenuItem(item.text)
                    for sub_item in submenu_items:
                        submenu.add(sub_item)
                    rumps_items.append(submenu)
                else:
                    # Create regular menu item
                    menu_item = rumps.MenuItem(
                        item.text, 
                        callback=self._create_callback_wrapper(item) if item.callback else None
                    )
                    
                    # Set enabled state
                    if not item.enabled:
                        menu_item.set_callback(None)
                        
                    # Set checked state
                    if item.checked:
                        menu_item.state = 1
                    else:
                        menu_item.state = 0
                        
                    rumps_items.append(menu_item)
                    
        return rumps_items
    
    def _create_callback_wrapper(self, item):
        """Create a callback wrapper that matches our interface."""
        def wrapper(sender):
            if item.callback:
                item.callback(None, item)
        return wrapper
    
    def show_balloon_notification(self, title, message, timeout=5000):
        """
        Show a notification on macOS using the Notification Center.
        """
        try:
            # Use AppleScript to show notification
            script = f'''
            display notification "{message}" with title "{title}" subtitle "{self.app_name}"
            '''
            subprocess.run(["osascript", "-e", script], check=True)
        except Exception as e:
            print(f"Failed to show notification: {e}")
    
    def exit_app(self, _, __):
        """Exit the application."""
        if self.app:
            rumps.quit_application()
    
    def run(self):
        """
        Run the tray application.
        """
        if not RUMPS_AVAILABLE:
            raise RuntimeError("rumps is not available")
            
        # Create the rumps app
        self.app = rumps.App(self.app_name, icon=self.icon_path, quit_button=None)
        
        # Build the menu
        menu = self.create_menu()
        menu_items = self.build_rumps_menu(menu.items)
        
        # Add menu items to the app
        for item in menu_items:
            self.app.menu.add(item)
        
        # Start the app
        self.app.run()
    
    def update_menu(self):
        """
        Update the menu by rebuilding it.
        """
        if self.app:
            # Clear existing menu
            self.app.menu.clear()
            
            # Rebuild menu
            menu = self.create_menu()
            menu_items = self.build_rumps_menu(menu.items)
            
            # Add updated menu items
            for item in menu_items:
                self.app.menu.add(item)


# Create a factory function to get the appropriate tray class
def get_system_tray_class():
    """
    Get the appropriate system tray class for the current platform.
    """
    system = platform.system()
    
    if system == "Darwin":  # macOS
        return MacOSSystemTray
    elif system == "Windows":
        from .system_tray import SystemTray
        return SystemTray
    else:
        raise NotImplementedError(f"System tray not implemented for {system}")
