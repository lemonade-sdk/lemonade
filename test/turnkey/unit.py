"""
Miscellaneous unit tests
"""

import unittest
import os
import sys
import turnkeyml.common.filesystem as filesystem
import turnkeyml.common.build as build
from turnkeyml.cli.parser_helpers import decode_args, encode_args


class Testing(unittest.TestCase):
    def test_000_models_dir(self):
        """
        Make sure that filesystem.MODELS_DIR points to turnkey_install/models
        """

        # Make sure the path is valid
        assert os.path.isdir(filesystem.MODELS_DIR)

        # Make sure the readme and a couple of corpora are in the directory
        models = os.listdir(filesystem.MODELS_DIR)
        assert "selftest" in models
        assert "transformers" in models
        assert "readme.md" in models

    def test_001_logger(self):
        """
        Ensure the logger stores both stdout and stderr and is also fail-safe
        """

        # Initialize messages used in the test
        logfile_path = "unit_log.txt"
        inside_stdout_msg = "This stdout should be inside the log file"
        inside_sterr_msg = "This stderr should be inside the log file"
        outside_stdout_msg = "This stdout should be outside the log file"
        outside_stderr_msg = "This stderr should be outside the log file"
        traceback_error_msg = "Tracebacks should be inside the log file"

        # Perform basic test (no exceptions inside logger)
        with build.Logger("", logfile_path):
            print(inside_stdout_msg, file=sys.stdout)
            print(inside_sterr_msg, file=sys.stderr)
        print(outside_stdout_msg, file=sys.stdout)
        print(outside_stderr_msg, file=sys.stderr)

        # Make sure we captured everything we intended to capture
        with open(logfile_path, "r", encoding="utf-8") as file:
            log_contents = file.read()
        assert inside_stdout_msg in log_contents
        assert inside_sterr_msg in log_contents
        assert outside_stdout_msg not in log_contents
        assert outside_stderr_msg not in log_contents

        # Perform test with exceptions inside the logger
        with self.assertRaises(ValueError):
            with build.Logger("", logfile_path):
                print(inside_stdout_msg, file=sys.stdout)
                print(inside_sterr_msg, file=sys.stderr)
                raise ValueError(traceback_error_msg)
        print(outside_stdout_msg, file=sys.stdout)
        print(outside_stderr_msg, file=sys.stderr)

        # Make sure we captured everything we intended to capture
        with open(logfile_path, "r", encoding="utf-8") as file:
            log_contents = file.read()
        assert inside_stdout_msg in log_contents
        assert inside_sterr_msg in log_contents
        assert traceback_error_msg in log_contents
        assert outside_stdout_msg not in log_contents
        assert outside_stderr_msg not in log_contents

    def test_002_args_encode_decode(self):
        """
        Test the encoding and decoding of arguments that follow the
        ["arg1::[value1,value2]","arg2::value1","flag_arg"]' format
        """
        encoded_value = ["arg1::[value1,value2]", "arg2::value1", "flag_arg"]
        decoded_value = decode_args(encoded_value)
        reencoded_value = encode_args(decoded_value)
        assert (
            reencoded_value == encoded_value
        ), f"input: {encoded_value}, decoded: {decoded_value}, reencoded_value: {reencoded_value}"


if __name__ == "__main__":
    unittest.main()
