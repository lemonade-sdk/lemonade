"""
Usage: python server_unit.py
"""

import unittest
from lemonade.tools.server.tool_calls import extract_tool_calls


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
        tool_calls, message = extract_tool_calls(pattern1, mock_special_tokens)
        assert tool_calls == expected_tool_calls
        assert message == expected_message

        # Pattern 2: [TOOL_CALLS] [ {...} ] block
        pattern2 = """
        The tool call is:
        [TOOL_CALLS] [{"name": "get_current_weather", "arguments": {"location": "Paris"}}]"""
        mock_special_tokens = {
            "1": Token("[TOOL_CALLS]"),
        }
        tool_calls, message = extract_tool_calls(pattern2, mock_special_tokens)
        assert tool_calls == expected_tool_calls
        assert message == expected_message

        # Pattern 3: Plain Json
        pattern3 = """
        {"name": "get_current_weather", "arguments": {"location": "Paris"}}
        """
        mock_special_tokens = {}
        tool_calls, message = extract_tool_calls(pattern3, mock_special_tokens)
        assert tool_calls == expected_tool_calls

        # Pattern 4: Json array
        pattern4 = """
        [
            {"name": "get_current_weather", "arguments": {"location": "Paris"}}
        ]
        """
        mock_special_tokens = {}
        tool_calls, message = extract_tool_calls(pattern4, mock_special_tokens)


if __name__ == "__main__":
    unittest.main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
