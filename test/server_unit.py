"""
Usage: python server_unit.py
"""

import unittest
from lemonade.tools.server.tool_calls import extract_tool_calls, get_tool_call_pattern
import os
import tempfile
import platform

# Only import system tray related modules on Windows
if platform.system() == "Windows":
    from lemonade.tools.server.utils.system_tray import SystemTray, Menu, MenuItem
    from lemonade.tools.server.tray import LemonadeTray


# Mock the tokenizer's added_tokens_decoder
# This is used to avoid the need to download/instantiate multiple models
class Token:
    def __init__(self, content):
        self.content = content


class Testing(unittest.IsolatedAsyncioTestCase):

    def test_001_tool_extraction(self):

        # Expected tool calls and message
        expected_tool_calls = [
            {"name": "get_current_weather", "arguments": {"location": "Paris"}}
        ]
        expected_message = "The tool call is:"

        # Pattern 1: <tool_call>...</tool_call> block
        pattern1 = """
        The tool call is:
        <tool_call>
        {"name": "get_current_weather", "arguments": {"location": "Paris"}}
        </tool_call>
        """
        mock_special_tokens = {
            "1": Token("<tool_call>"),
            "2": Token("</tool_call>"),
        }
        tool_call_pattern = get_tool_call_pattern(mock_special_tokens)
        tool_calls, message = extract_tool_calls(pattern1, tool_call_pattern)
        assert tool_calls == expected_tool_calls
        assert message == expected_message

        # Pattern 2: [TOOL_CALLS] [ {...} ] block
        pattern2 = """
        The tool call is:
        [TOOL_CALLS] [{"name": "get_current_weather", "arguments": {"location": "Paris"}}]"""
        mock_special_tokens = {
            "1": Token("[TOOL_CALLS]"),
        }
        tool_call_pattern = get_tool_call_pattern(mock_special_tokens)
        tool_calls, message = extract_tool_calls(pattern2, tool_call_pattern)
        assert tool_calls == expected_tool_calls
        assert message == expected_message

        # Pattern 3: Plain Json
        pattern3 = """
        {"name": "get_current_weather", "arguments": {"location": "Paris"}}
        """
        mock_special_tokens = {}
        tool_call_pattern = None
        tool_calls, message = extract_tool_calls(pattern3, tool_call_pattern)
        assert tool_calls == expected_tool_calls

        # Pattern 4: Json array
        pattern4 = """
        [
            {"name": "get_current_weather", "arguments": {"location": "Paris"}}
        ]
        """
        mock_special_tokens = {}
        tool_call_pattern = None
        tool_calls, message = extract_tool_calls(pattern4, tool_call_pattern)
        assert tool_calls == expected_tool_calls


@unittest.skipIf(
    platform.system() != "Windows", "System tray tests only run on Windows"
)
class SystemTrayTesting(unittest.TestCase):
    def setUp(self):
        # Create a temporary icon file for testing
        self.temp_dir = tempfile.mkdtemp()
        self.icon_path = os.path.join(self.temp_dir, "test.ico")
        with open(self.icon_path, "wb") as f:
            f.write(b"dummy icon data")

    def test_001_system_tray_basics(self):
        """Test basic system tray functionality including menu creation and initialization"""
        # Test SystemTray initialization
        tray = SystemTray("TestApp", self.icon_path)
        self.assertEqual(tray.app_name, "TestApp")
        self.assertEqual(tray.icon_path, self.icon_path)
        self.assertIsNone(tray.hwnd)
        self.assertIsNone(tray.notify_id)
        self.assertEqual(tray.next_menu_id, 1000)
        self.assertEqual(len(tray.menu_items), 0)

        # Test menu creation and structure
        callback = lambda x, y: None
        menu = Menu(
            MenuItem("Item 1", callback=callback),
            Menu.SEPARATOR,
            MenuItem("Item 2", enabled=False, bitmap_path="test.bmp"),
            MenuItem(
                "Submenu", submenu=Menu(MenuItem("Sub Item 1"), MenuItem("Sub Item 2"))
            ),
        )

        # Verify menu structure and item properties
        self.assertEqual(len(menu.items), 4)
        self.assertEqual(menu.items[0].text, "Item 1")
        self.assertEqual(menu.items[0].callback, callback)
        self.assertEqual(menu.items[1], Menu.SEPARATOR)
        self.assertEqual(menu.items[2].text, "Item 2")
        self.assertFalse(menu.items[2].enabled)
        self.assertEqual(menu.items[2].bitmap_path, "test.bmp")
        self.assertEqual(menu.items[3].text, "Submenu")
        self.assertEqual(len(menu.items[3].submenu.items), 2)

    def test_002_get_latest_version(self):
        """Test fetching latest version from GitHub"""
        # Create a minimal LemonadeTray instance just for testing get_latest_version
        tray = LemonadeTray(log_file="test.log", port=8000, server_factory=None)
        version, installer_url = tray.get_latest_version()

        # Print version info for debugging
        self.assertIsNotNone(version)
        self.assertTrue(all(c.isdigit() or c == "." for c in version))
        self.assertEqual(len(version.split(".")), 3)  # Should have 3 numbers

        # Print installer URL info for debugging
        self.assertIsNotNone(installer_url)
        self.assertTrue(installer_url.startswith("https://"))
        self.assertTrue(installer_url.endswith(".exe"))
        self.assertIn("Lemonade_Server_Installer", installer_url)

    def tearDown(self):
        # Clean up temporary files
        if os.path.exists(self.icon_path):
            os.remove(self.icon_path)
        os.rmdir(self.temp_dir)


if __name__ == "__main__":
    unittest.main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
