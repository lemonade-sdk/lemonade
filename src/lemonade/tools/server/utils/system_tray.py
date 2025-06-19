import os
import sys

# Add pywin32_system32 dlls (missing from embeddable Python executable)
pywin32_system32 = os.path.join(sys.prefix, "Lib", "site-packages", "pywin32_system32")
os.add_dll_directory(pywin32_system32)

from win32 import win32gui, win32api
import win32.lib.win32con as win32con
import ctypes
import psutil
from typing import Dict, Set, Callable, List, Optional, Tuple, Any

# Windows message constants
WM_USER = 0x0400
WM_TRAYICON = WM_USER + 1
WM_COMMAND = 0x0111


class MenuItem:
    def __init__(
        self,
        text: str,
        callback: Optional[Callable] = None,
        enabled: bool = True,
        submenu=None,
        bitmap_path: Optional[str] = None,
    ):
        self.text = text
        self.callback = callback
        self.enabled = enabled
        self.submenu = submenu
        self.bitmap_path = bitmap_path
        self.id = None  # Will be set when menu is created
        self.bitmap_handle = None


class Menu:
    SEPARATOR = "SEPARATOR"

    def __init__(self, *items):
        self.items = list(items)


class SystemTray:
    """
    Generic system tray implementation for Windows.
    """

    def __init__(self, app_name: str, icon_path: str):
        self.app_name = app_name
        self.icon_path = icon_path
        self.hwnd = None
        self.hinst = win32api.GetModuleHandle(None)
        self.class_atom = None
        self.notify_id = None
        self.menu_handle = None
        self.menu_items = []  # Store menu items with their IDs
        self.next_menu_id = 1000  # Starting ID for menu items

        # Message map for window procedure
        self.message_map = {
            win32con.WM_DESTROY: self.on_destroy,
            win32con.WM_COMMAND: self.on_command,
            WM_TRAYICON: self.on_tray_icon,
        }

    def register_window_class(self):
        """
        Register the window class for the tray icon.
        """
        window_class = win32gui.WNDCLASS()
        window_class.hInstance = self.hinst
        window_class.lpszClassName = f"{self.app_name}TrayClass"
        window_class.style = win32con.CS_VREDRAW | win32con.CS_HREDRAW
        window_class.hCursor = win32api.LoadCursor(0, win32con.IDC_ARROW)
        window_class.hbrBackground = win32con.COLOR_WINDOW
        window_class.lpfnWndProc = self.window_proc

        self.class_atom = win32gui.RegisterClass(window_class)
        return self.class_atom

    def create_window(self):
        """
        Create a hidden window to receive messages.
        """
        style = win32con.WS_OVERLAPPED | win32con.WS_SYSMENU
        self.hwnd = win32gui.CreateWindow(
            self.class_atom,
            f"{self.app_name} Tray",
            style,
            0,
            0,
            win32con.CW_USEDEFAULT,
            win32con.CW_USEDEFAULT,
            0,
            0,
            self.hinst,
            None,
        )

        # Set the AppUserModelID to identify our app to Windows
        try:
            SetCurrentProcessExplicitAppUserModelID = (
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID
            )
            SetCurrentProcessExplicitAppUserModelID(ctypes.c_wchar_p(self.app_name))
        except Exception as e:
            print(f"Failed to set AppUserModelID: {e}")

        win32gui.UpdateWindow(self.hwnd)
        return self.hwnd

    def add_tray_icon(self):
        """
        Add the tray icon to the system tray.
        """
        # Load the .ico file
        icon_flags = win32con.LR_LOADFROMFILE | win32con.LR_DEFAULTSIZE
        hicon = win32gui.LoadImage(
            self.hinst, str(self.icon_path), win32con.IMAGE_ICON, 0, 0, icon_flags
        )

        flags = win32gui.NIF_ICON | win32gui.NIF_MESSAGE | win32gui.NIF_TIP
        nid = (self.hwnd, 0, flags, WM_TRAYICON, hicon, self.app_name)
        win32gui.Shell_NotifyIcon(win32gui.NIM_ADD, nid)
        self.notify_id = nid

    def show_balloon_notification(self, title, message, timeout=5000):
        """
        Show a balloon notification from the tray icon.
        """
        if self.notify_id:
            # The notify_id is a tuple, we need to extract its elements
            hwnd, id, flags, callback_msg, hicon, tip = self.notify_id

            # Create a new notification with the balloon info
            flags |= win32gui.NIF_INFO

            # NIIF_USER (0x4) tells Windows to use our custom icon in the notification
            info_flags = 0x4  # NIIF_USER

            # Create the notification data structure with our custom icon flag
            nid = (
                hwnd,
                id,
                flags,
                callback_msg,
                hicon,
                self.app_name,
                message,
                timeout,
                title,
                info_flags,
            )

            # Show the notification
            win32gui.Shell_NotifyIcon(win32gui.NIM_MODIFY, nid)

    def window_proc(self, hwnd, message, wparam, lparam):
        """
        Window procedure to handle window messages.
        """
        if message in self.message_map:
            return self.message_map[message](hwnd, message, wparam, lparam)
        return win32gui.DefWindowProc(hwnd, message, wparam, lparam)

    def on_destroy(self, hwnd, message, wparam, lparam):
        """
        Handle window destruction.
        """
        if self.notify_id:
            win32gui.Shell_NotifyIcon(win32gui.NIM_DELETE, self.notify_id)

        # Clean up bitmap resources
        for item_id, item in self.menu_items:
            if hasattr(item, "bitmap_handle") and item.bitmap_handle:
                win32gui.DeleteObject(item.bitmap_handle)

        win32gui.PostQuitMessage(0)
        return 0

    def on_command(self, hwnd, message, wparam, lparam):
        """
        Handle menu commands.
        """
        menu_id = win32gui.LOWORD(wparam)

        # Find the menu item with this ID and execute its callback
        for item_id, item_obj in self.menu_items:
            if item_id == menu_id and item_obj.callback:
                item_obj.callback(None, item_obj)
                break

        return 0

    def on_tray_icon(self, hwnd, message, wparam, lparam):
        """
        Handle tray icon events.
        """
        if lparam == win32con.WM_RBUTTONUP or lparam == win32con.WM_LBUTTONUP:
            # Show context menu on right-click and left-click
            self.show_menu()
        return 0

    def create_menu_item(self, menu_handle, item, pos):
        """
        Create a menu item and add it to the menu.
        """
        if item == Menu.SEPARATOR:
            win32gui.AppendMenu(menu_handle, win32con.MF_SEPARATOR, 0, "")
            return pos + 1

        # Assign a unique ID to this menu item
        item.id = self.next_menu_id
        self.next_menu_id += 1

        # Store the menu item with its ID for later lookup
        self.menu_items.append((item.id, item))

        # Create the menu item
        flags = win32con.MF_STRING

        if not item.enabled:
            flags |= win32con.MF_GRAYED

        # Add checkmark if this is a selected item
        if hasattr(item, "checked") and item.checked:
            flags |= win32con.MF_CHECKED

        # Handle submenu
        if item.submenu:
            submenu_handle = win32gui.CreatePopupMenu()
            submenu_pos = 0
            for submenu_item in item.submenu.items:
                submenu_pos = self.create_menu_item(
                    submenu_handle, submenu_item, submenu_pos
                )

            # Add submenu to parent menu
            win32gui.AppendMenu(
                menu_handle, win32con.MF_POPUP | flags, submenu_handle, item.text
            )
        else:
            # Add regular menu item first
            win32gui.AppendMenu(menu_handle, flags, item.id, item.text)

            # Then add bitmap if provided (as a separate step)
            if item.bitmap_path:
                try:
                    # Load the bitmap
                    icon_flags = win32con.LR_LOADFROMFILE | win32con.LR_DEFAULTSIZE
                    item.bitmap_handle = win32gui.LoadImage(
                        self.hinst,
                        str(item.bitmap_path),
                        win32con.IMAGE_BITMAP,
                        0,
                        0,
                        icon_flags,
                    )

                    if item.bitmap_handle:
                        # Set the bitmap for checked/unchecked states
                        # Using the same bitmap for both states
                        win32gui.SetMenuItemBitmaps(
                            menu_handle,
                            item.id,
                            win32con.MF_BYCOMMAND,
                            item.bitmap_handle,
                            item.bitmap_handle,
                        )
                    else:
                        print(f"Failed to load bitmap from {item.bitmap_path}")
                except Exception as e:
                    print(f"Error adding bitmap to menu item: {e}")

        return pos + 1

    def build_menu(self, menu_items):
        """
        Build a menu from a list of menu items.
        """
        # Create a new menu
        menu_handle = win32gui.CreatePopupMenu()
        pos = 0

        # Clear previous menu items
        self.menu_items = []
        self.next_menu_id = 1000

        # Add each item to the menu
        for item in menu_items:
            pos = self.create_menu_item(menu_handle, item, pos)

        return menu_handle

    def show_menu(self):
        """
        Show the context menu.
        """
        # Create menu based on current state
        menu = self.create_menu()
        menu_handle = self.build_menu(menu.items)

        # Get cursor position
        pos = win32gui.GetCursorPos()

        # Make our window the foreground window
        try:
            win32gui.SetForegroundWindow(self.hwnd)
        except Exception:
            # Ignore errors when setting foreground window
            # Those are common when the tray icon is already open
            pass

        # Display the menu
        win32gui.TrackPopupMenu(
            menu_handle,
            win32con.TPM_LEFTALIGN | win32con.TPM_RIGHTBUTTON,
            pos[0],
            pos[1],
            0,
            self.hwnd,
            None,
        )

        # Required by Windows
        win32gui.PostMessage(self.hwnd, win32con.WM_NULL, 0, 0)

    def create_menu(self):
        """
        Create the context menu based on current state. Override in subclass.
        """
        return Menu(MenuItem("Exit", self.exit_app))

    def exit_app(self, _, __):
        """Exit the application."""
        win32gui.DestroyWindow(self.hwnd)

    def update_menu(self):
        """
        Update the menu (will be shown on next right-click).
        """
        if self.hwnd:
            win32gui.InvalidateRect(self.hwnd, None, True)

    def message_loop(self):
        """
        Run the Windows message loop.
        """
        win32gui.PumpMessages()

    def run(self):
        """
        Run the tray application.
        """
        # Register window class and create window
        self.register_window_class()
        self.create_window()

        # Add tray icon
        self.add_tray_icon()

        # Run the message loop in the main thread
        self.message_loop()

    def setup_console_control_handler(self, logger=None):
        """
        Set up Windows console control handler for CTRL+C events.
        This handles graceful shutdown when the console window is closed or CTRL+C is pressed.
        """

        def console_ctrl_handler(ctrl_type):
            if ctrl_type in (0, 2):  # CTRL_C_EVENT or CTRL_CLOSE_EVENT
                if logger:
                    logger.info(
                        "Received console control event, shutting down gracefully"
                    )
                # Post a quit message to the main thread instead of calling exit_app directly
                # This avoids thread access issues
                if self.hwnd:
                    win32gui.PostMessage(self.hwnd, win32con.WM_CLOSE, 0, 0)
                return True
            return False

        # Define the handler function type
        HANDLER_ROUTINE = ctypes.WINFUNCTYPE(
            ctypes.wintypes.BOOL, ctypes.wintypes.DWORD
        )
        handler = HANDLER_ROUTINE(console_ctrl_handler)

        # Set the console control handler
        ctypes.windll.kernel32.SetConsoleCtrlHandler(handler, True)

        return handler  # Return handler to keep it in scope
