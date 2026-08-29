#!/usr/bin/env python3
"""
CPU-runnable unit tests for device family matching logic (device_matches_constraint).

These tests replicate the C++ logic for matching a device family against a set of
allowed families, including support for wildcard 'X' at the end of a family name.
"""

import unittest

# ---------------------------------------------------------------------------
# Python replica of system_info.cpp::device_matches_constraint()
# ---------------------------------------------------------------------------


def device_matches_constraint(device_family: str, allowed_families: set) -> bool:
    if not allowed_families:
        return True  # Empty = all families allowed

    if device_family in allowed_families:
        return True

    for af in allowed_families:
        if len(af) > 1 and af.endswith("X"):
            prefix = af[:-1]
            if device_family.startswith(prefix):
                return True

    return False


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestDeviceFamilyMatching(unittest.TestCase):
    def test_wildcard_matching(self):
        # gfx1103 should match gfx110X
        self.assertTrue(device_matches_constraint("gfx1103", {"gfx110X"}))
        # gfx1201 should match gfx120X
        self.assertTrue(device_matches_constraint("gfx1201", {"gfx120X"}))

    def test_exact_matching(self):
        # gfx1151 should match gfx1151
        self.assertTrue(device_matches_constraint("gfx1151", {"gfx1151"}))
        # gfx1152 should match gfx1152
        self.assertTrue(device_matches_constraint("gfx1152", {"gfx1152"}))

    def test_non_matching(self):
        # gfx1151 should NOT match gfx110X
        self.assertFalse(device_matches_constraint("gfx1151", {"gfx110X"}))

    def test_empty_allowed_families(self):
        # Empty allowed_families should match everything
        self.assertTrue(device_matches_constraint("gfx1151", set()))

    def test_multiple_allowed_families(self):
        # Should match if any in the set match
        self.assertTrue(device_matches_constraint("gfx1103", {"gfx103X", "gfx110X"}))
        self.assertTrue(device_matches_constraint("gfx1201", {"gfx110X", "gfx120X"}))
        self.assertFalse(device_matches_constraint("gfx1151", {"gfx103X", "gfx110X"}))


if __name__ == "__main__":
    unittest.main()
