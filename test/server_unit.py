"""
Usage: python server_unit.py
"""

import os
import unittest
import tempfile
import platform
from lemonade.tools.server.tool_calls import extract_tool_calls, get_tool_call_pattern
from lemonade.tools.llamacpp.utils import parse_checkpoint, identify_gguf_models

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

    def test_002_gguf_model_identification(self):

        # Case 1. Full filename: exact file to download
        checkpoint, variant = parse_checkpoint(
            "unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q3_K_S.gguf"
        )
        core_files, sharded_files = identify_gguf_models(checkpoint, variant, None)
        assert core_files == {"variant": "Qwen3-0.6B-Q3_K_S.gguf"}
        assert sharded_files == []

        # Case 2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
        checkpoint, variant = parse_checkpoint("unsloth/Qwen3-0.6B-GGUF")
        core_files, sharded_files = identify_gguf_models(checkpoint, variant, None)
        assert core_files == {"variant": "Qwen3-0.6B-BF16.gguf"}
        assert sharded_files == []

        # Case 3. Quantization variant: find a single file ending with the variant name (case insensitive)
        checkpoint, variant = parse_checkpoint("unsloth/Qwen3-0.6B-GGUF:Q3_K_S")
        core_files, sharded_files = identify_gguf_models(checkpoint, variant, None)
        assert core_files == {"variant": "Qwen3-0.6B-Q3_K_S.gguf"}
        assert sharded_files == []

        # Case 4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)
        checkpoint, variant = parse_checkpoint(
            "unsloth/Llama-4-Scout-17B-16E-Instruct-GGUF:Q4_K_S"
        )
        core_files, sharded_files = identify_gguf_models(checkpoint, variant, None)
        assert core_files == {
            "variant": "Q4_K_S/Llama-4-Scout-17B-16E-Instruct-Q4_K_S-00001-of-00002.gguf"
        }
        assert sharded_files == [
            "Q4_K_S/Llama-4-Scout-17B-16E-Instruct-Q4_K_S-00001-of-00002.gguf",
            "Q4_K_S/Llama-4-Scout-17B-16E-Instruct-Q4_K_S-00002-of-00002.gguf",
        ]

        # Common on all cases: mmproj file
        checkpoint, variant = parse_checkpoint(
            "unsloth/Qwen2.5-VL-7B-Instruct-GGUF:BF16"
        )
        core_files, sharded_files = identify_gguf_models(
            checkpoint, variant, "mmproj-BF16.gguf"
        )
        assert core_files == {
            "variant": "Qwen2.5-VL-7B-Instruct-BF16.gguf",
            "mmproj": "mmproj-BF16.gguf",
        }
        assert sharded_files == []


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
