import platform
import subprocess
from typing import Callable, Optional

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
    """
    Cross-platform menu item representation.
    """

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
    """
    Cross-platform menu representation.
    """

    SEPARATOR = "SEPARATOR"

    def __init__(self, *items):
        self.items = list(items)


class MacOSSystemTray:
    """
    macOS-specific system tray implementation using rumps.
    """

    def __init__(self, app_name: str, icon_path: str):
        self._check_rumps_availability()

        self.app_name = app_name
        self.icon_path = icon_path
        self.app = None
        self.menu_callbacks = {}
        self._menu_update_timer = None

    def _check_rumps_availability(self):
        """Check if rumps is available and raise error if not."""
        if not RUMPS_AVAILABLE:
            raise ImportError("rumps library is required for macOS tray support")

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
                        callback=(
                            self._create_callback_wrapper(item)
                            if item.callback
                            else None
                        ),
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

        def wrapper(sender):  # pylint: disable=unused-argument
            if item.callback:
                item.callback(None, item)

        return wrapper

    def show_balloon_notification(
        self, title, message, timeout=5000
    ):  # pylint: disable=unused-argument
        """
        Show a notification on macOS using the Notification Center.
        Falls back to console output if AppleScript fails.
        """
        try:
            # Escape quotes in message and title for AppleScript
            escaped_title = title.replace('"', '\\"')
            escaped_message = message.replace('"', '\\"')
            escaped_app_name = self.app_name.replace('"', '\\"')

            # Use AppleScript to show notification
            script = (
                f'display notification "{escaped_message}" '
                f'with title "{escaped_title}" subtitle "{escaped_app_name}"'
            )
            subprocess.run(
                ["osascript", "-e", script], check=True, capture_output=True, text=True
            )
        except FileNotFoundError:
            # osascript not available, fallback to console
            print(f"[{self.app_name}] {title}: {message}")
        except subprocess.CalledProcessError as e:
            # AppleScript failed, fallback to console
            print(f"[{self.app_name}] {title}: {message}")
            print(f"Warning: Failed to show notification via AppleScript: {e}")
        except Exception as e:  # pylint: disable=broad-exception-caught
            # Any other error, fallback to console
            print(f"[{self.app_name}] {title}: {message}")
            print(f"Warning: Failed to show notification: {e}")

    def exit_app(self, _, __):
        """Exit the application."""
        if self.app:
            rumps.quit_application()

    def run(self):
        """
        Run the tray application.
        """
        self._check_rumps_availability()

        try:
            # Create the rumps app
            self.app = rumps.App(self.app_name, icon=self.icon_path, quit_button=None)

            # Build the initial menu
            self.refresh_menu()

            # Set up a timer to refresh menu periodically (every 3 seconds)
            # This provides a good balance between responsiveness and performance
            self._setup_menu_refresh_timer()

            # Call the on_ready hook if available (for compatibility with tray.py)
            if hasattr(self, "on_ready") and callable(getattr(self, "on_ready", None)):
                getattr(self, "on_ready")()

            # Start the app
            self.app.run()
        except Exception as e:
            raise RuntimeError(f"Failed to start macOS tray application: {e}") from e

    def refresh_menu(self):
        """
        Refresh the menu by rebuilding it with current state.
        """
        if not self.app:
            return

        # Clear existing menu
        self.app.menu.clear()

        # Build fresh menu with current state
        menu = self.create_menu()
        menu_items = self.build_rumps_menu(menu.items)

        # Add updated menu items
        for item in menu_items:
            self.app.menu.add(item)

    def _setup_menu_refresh_timer(self):
        """
        Set up a timer to periodically refresh the menu.
        """
        if not self.app:
            return

        # Create a timer that refreshes the menu every 3 seconds
        @rumps.timer(3)
        def refresh_menu_timer(sender):  # pylint: disable=unused-argument
            self.refresh_menu()

        # Store reference to prevent garbage collection
        self._menu_update_timer = refresh_menu_timer

    def update_menu(self):
        """
        Update the menu by rebuilding it.
        """
        self.refresh_menu()
