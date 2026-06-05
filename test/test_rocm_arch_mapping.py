#!/usr/bin/env python3
"""
CPU-runnable unit tests for ROCm compute architecture mapping logic.

These tests replicate the C++ identify_rocm_arch_from_name() function so the
mapping can be validated without AMD hardware or a running server.

Run with: python -m pytest test/test_rocm_arch_mapping.py
      or: python test/test_rocm_arch_mapping.py
"""

import unittest
import re

ROCM_SUPPORTED_ARCHS = {
    "gfx908",
    "gfx90a",
    "gfx942",
    "gfx101X",
    "gfx103X",
    "gfx110X",
    "gfx120X",
    "gfx1100",
    "gfx1101",
    "gfx1102",
    "gfx1150",
    "gfx1151",
    "gfx1152",
    "gfx1200",
    "gfx1201",
    "gfx1010",
    "gfx1011",
    "gfx1012",
    "gfx1030",
    "gfx1031",
    "gfx1032",
    "gfx1033",
    "gfx1034",
    "gfx1035",
    "gfx1036",
}

ROCM_ARCH_MAPPING = {
    # RDNA1 family (gfx101X)
    "gfx1010": "gfx101X",
    "gfx1011": "gfx101X",
    "gfx1012": "gfx101X",
    # RDNA2 family (gfx103X)
    "gfx1030": "gfx103X",
    "gfx1031": "gfx103X",
    "gfx1032": "gfx103X",
    "gfx1034": "gfx103X",
    "gfx1033": "gfx103X",
    "gfx1035": "gfx103X",
    "gfx1036": "gfx103X",
    # RDNA3 family (gfx110X)
    "gfx1100": "gfx110X",
    "gfx1101": "gfx110X",
    "gfx1102": "gfx110X",
    "gfx1103": "gfx110X",
    # RDNA3.5 iGPUs - explicit binary names (no family mapping)
    "gfx1150": "gfx1150",
    "gfx1151": "gfx1151",
    # RDNA4 family (gfx120X)
    "gfx1200": "gfx120X",
    "gfx1201": "gfx120X",
}


def identify_rocm_arch_from_name(device_name: str) -> str:
    """Python replica of system_info.cpp::identify_rocm_arch_from_name()"""
    device_lower = device_name.lower()

    # Search for explicit gfxXXXX pattern
    gfx_match = re.search(r"(gfx\d{4})", device_lower)
    if gfx_match:
        arch = gfx_match.group(1)
        if arch in ROCM_ARCH_MAPPING:
            return ROCM_ARCH_MAPPING[arch]
        return arch

    # Linux KFD topology node version mapping (numeric strings)
    if device_lower.isdigit():
        if len(device_lower) >= 4:
            major = device_lower[0:2]
            try:
                minor = str(int(device_lower[2:4]))
                revision = str(int(device_lower[4:6]))
            except (ValueError, IndexError):
                minor = "0"
                revision = "0"

            arch = f"gfx{major}{minor}{revision}"
            if arch in ROCM_ARCH_MAPPING:
                return ROCM_ARCH_MAPPING[arch]
            return arch

    # Friendly name mapping
    if "radeon" not in device_lower and "amd" not in device_lower:
        return ""

    # STX Halo iGPUs (gfx1151 architecture)
    if (
        "8050s" in device_lower
        or "8060s" in device_lower
        or "device 1586" in device_lower
    ):
        return "gfx1151"

    # STX Point iGPUs (gfx1150 architecture)
    if "880m" in device_lower or "890m" in device_lower:
        return "gfx1150"

    # RDNA4 GPUs (gfx120X architecture)
    if "r9700" in device_lower or "9060" in device_lower or "9070" in device_lower:
        return "gfx120X"

    # RDNA3 GPUs (gfx110X architecture)
    if (
        "7700" in device_lower
        or "7800" in device_lower
        or "7900" in device_lower
        or "v710" in device_lower
    ):
        return "gfx110X"

    # RDNA2 GPUs (gfx103X architecture)
    if (
        "6800" in device_lower
        or "6700" in device_lower
        or "6600" in device_lower
        or "6500" in device_lower
    ):
        return "gfx103X"

    # RDNA1 GPUs (gfx101X architecture)
    if "5700" in device_lower or "5600" in device_lower or "5500" in device_lower:
        return "gfx101X"

    return ""


class TestIdentifyRocmArchFromName(unittest.TestCase):
    def test_explicit_gfx_names(self):
        cases = [
            ("AMD Radeon RX 7900 XTX (gfx1100)", "gfx110X"),
            ("gfx1030", "gfx103X"),
            ("gfx1010", "gfx101X"),
            ("gfx1012", "gfx101X"),
            ("gfx1151", "gfx1151"),
        ]
        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(identify_rocm_arch_from_name(name), expected)

    def test_linux_kfd_numeric_strings(self):
        cases = [
            ("100100", "gfx101X"),  # gfx1010 -> gfx101X (RDNA1 RX 5700 series)
            ("100101", "gfx101X"),  # gfx1011 -> gfx101X
            ("100102", "gfx101X"),  # gfx1012 -> gfx101X
            ("100300", "gfx103X"),  # gfx1030 -> gfx103X (RDNA2 RX 6000 series)
            ("110000", "gfx110X"),  # gfx1100 -> gfx110X (RDNA3 RX 7000 series)
            ("110500", "gfx1150"),  # gfx1150 -> gfx1150 (RDNA3.5 880M/890M)
            ("110501", "gfx1151"),  # gfx1151 -> gfx1151 (RDNA3.5 8050S/8060S)
            ("120000", "gfx120X"),  # gfx1200 -> gfx120X (RDNA4 RX 9000 series)
        ]
        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(identify_rocm_arch_from_name(name), expected)

    def test_linux_kfd_numeric_strings_map_to_supported_archs(self):
        for name in ["100100", "100101", "100102"]:
            with self.subTest(name=name):
                arch = identify_rocm_arch_from_name(name)
                self.assertIn(arch, ROCM_SUPPORTED_ARCHS)

    def test_windows_friendly_names(self):
        cases = [
            ("AMD Radeon RX 5700 XT", "gfx101X"),
            ("AMD Radeon RX 5600 XT", "gfx101X"),
            ("AMD Radeon RX 5500 XT", "gfx101X"),
            ("AMD Radeon RX 6800 XT", "gfx103X"),
            ("AMD Radeon RX 7900 XT", "gfx110X"),
            ("AMD Radeon RX 9070 XT", "gfx120X"),
            ("AMD Radeon 880M Graphics", "gfx1150"),
            ("AMD Radeon 890M Graphics", "gfx1150"),
        ]
        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(identify_rocm_arch_from_name(name), expected)

    def test_non_amd_returns_empty(self):
        for name in ["NVIDIA GeForce RTX 4090", "Intel Arc A770", "Apple M3", ""]:
            with self.subTest(name=name):
                self.assertEqual(identify_rocm_arch_from_name(name), "")


if __name__ == "__main__":
    unittest.main()
