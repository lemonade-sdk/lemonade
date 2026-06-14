#!/usr/bin/env python3
"""
CPU-runnable unit tests for ROCm gfx arch detection logic.

These tests replicate the C++ identify_rocm_arch_from_name() function so the
mapping can be validated without AMD hardware or a running server.

Run with: python -m pytest test/test_rocm_arch_mapping.py
      or: python test/test_rocm_arch_mapping.py
"""

import re
import unittest

# ---------------------------------------------------------------------------
# Python replica of system_info.cpp::identify_rocm_arch_from_name()
# ---------------------------------------------------------------------------

# Family/constraint tokens as used in RECIPE_DEFS. CDNA entries are concrete arch
# strings; RDNA entries are wildcard family tokens (gfx103X etc.) matched by the
# name-based fallback, not by the KFD path which produces exact arches (gfx1030).
ROCM_SUPPORTED_FAMILIES = {
    "gfx908",   # CDNA1 (MI100) — exact arch
    "gfx90a",   # CDNA2 (MI200/MI210/MI250) — exact arch
    "gfx1150",  # Strix Point iGPU — exact arch
    "gfx1151",  # Strix Halo iGPU — exact arch
    "gfx1152",  # Krackan Point iGPU — exact arch
    "gfx103X",  # RDNA2 dGPUs — family token (name-based detection only)
    "gfx110X",  # RDNA3 dGPUs — family token
    "gfx120X",  # RDNA4 dGPUs — family token
}

_GFX_RE = re.compile(r"(gfx[0-9a-f]{3,4})")


def _kfd_version_to_gfx(version_str: str) -> str:
    """
    Convert a KFD decimal gfx_target_version string (all digits) to a gfx arch.
    Mirrors: snprintf(buf, sizeof(buf), "gfx%d%x%x", major, minor, step)
    """
    v = int(version_str)
    major = v // 10000
    minor = (v // 100) % 100
    step = v % 100
    return f"gfx{major:d}{minor:x}{step:x}"


def identify_rocm_arch_from_name(device_name: str) -> str:
    """
    Python replica of system_info.cpp::identify_rocm_arch_from_name().

    Priority order:
    1. Regex: direct gfx token in the string (e.g. "gfx90a")
    2. All-digit string: KFD gfx_target_version decimal
    3. CDNA name keywords (before radeon/amd guard)
    4. radeon/amd guard — returns "" for non-AMD names
    5. RDNA marketing name keywords
    """
    device_lower = device_name.lower()

    # 1. Direct gfx token
    m = _GFX_RE.search(device_lower)
    if m:
        return m.group(1)

    # 2. KFD decimal version (all digits)
    if device_lower and all(c.isdigit() for c in device_lower):
        return _kfd_version_to_gfx(device_lower)

    # 3. CDNA checks — before the radeon/amd guard so bare codenames work
    if "mi100" in device_lower or "arcturus" in device_lower:
        return "gfx908"

    if ("mi200" in device_lower or "mi210" in device_lower or
            "mi250" in device_lower or "aldebaran" in device_lower):
        return "gfx90a"

    # 4. Require "radeon" or "amd" for RDNA name matching
    if "radeon" not in device_lower and "amd" not in device_lower:
        return ""

    # 5. RDNA name keywords
    if any(kw in device_lower for kw in ("8050s", "8060s", "device 1586")):
        return "gfx1151"
    if any(kw in device_lower for kw in ("880m", "890m")):
        return "gfx1150"
    if any(kw in device_lower for kw in ("840m", "860m")):
        return "gfx1152"
    if any(kw in device_lower for kw in ("r9700", "9060", "9070")):
        return "gfx120X"
    if any(kw in device_lower for kw in ("7700", "7800", "7900", "v710")):
        return "gfx110X"
    if any(kw in device_lower for kw in ("6800", "6700", "6600", "6500")):
        return "gfx103X"

    return ""


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestKfdVersionToGfx(unittest.TestCase):
    """Test the KFD decimal gfx_target_version -> gfx arch conversion."""

    def test_cdna1_mi100(self):
        # gfx908: major=9, minor=0, step=8 -> 9*10000 + 0*100 + 8 = 90008
        self.assertEqual(_kfd_version_to_gfx("90008"), "gfx908")

    def test_cdna2_mi210(self):
        # gfx90a: major=9, minor=0, step=10(=0xa) -> 9*10000 + 0*100 + 10 = 90010
        self.assertEqual(_kfd_version_to_gfx("90010"), "gfx90a")

    def test_rdna2_gfx1030(self):
        # gfx1030: major=10, minor=3, step=0 -> 10*10000 + 3*100 + 0 = 100300
        self.assertEqual(_kfd_version_to_gfx("100300"), "gfx1030")

    def test_rdna3_gfx1100(self):
        # gfx1100: major=11, minor=0, step=0 -> 110000
        self.assertEqual(_kfd_version_to_gfx("110000"), "gfx1100")

    def test_strix_point_gfx1150(self):
        # gfx1150: "gfx%d%x%x" -> major=11, hex(minor)="5"->5, hex(step)="0"->0
        # KFD = 11*10000 + 5*100 + 0 = 110500
        self.assertEqual(_kfd_version_to_gfx("110500"), "gfx1150")

    def test_strix_halo_gfx1151(self):
        # gfx1151: major=11, hex(minor)="5"->5, hex(step)="1"->1
        # KFD = 11*10000 + 5*100 + 1 = 110501
        self.assertEqual(_kfd_version_to_gfx("110501"), "gfx1151")


class TestIdentifyRocmArchFromName(unittest.TestCase):
    """Test identify_rocm_arch_from_name() across all detection paths."""

    # -- Direct gfx token path --

    def test_direct_gfx_token_in_name(self):
        cases = [
            ("gfx908", "gfx908"),
            ("gfx90a", "gfx90a"),
            ("AMD Radeon gfx1030", "gfx1030"),
            ("gfx1150 iGPU", "gfx1150"),
        ]
        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(identify_rocm_arch_from_name(name), expected)

    # -- KFD numeric path --

    def test_kfd_numeric_cdna1(self):
        self.assertEqual(identify_rocm_arch_from_name("90008"), "gfx908")

    def test_kfd_numeric_cdna2(self):
        self.assertEqual(identify_rocm_arch_from_name("90010"), "gfx90a")

    def test_kfd_numeric_rdna2(self):
        self.assertEqual(identify_rocm_arch_from_name("100300"), "gfx1030")

    # -- CDNA name fallback (before radeon/amd guard) --

    def test_cdna1_full_name(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Instinct MI100"), "gfx908")

    def test_cdna1_bare_name(self):
        # Must work without "AMD" prefix — checked before the early guard
        self.assertEqual(identify_rocm_arch_from_name("MI100"), "gfx908")

    def test_cdna1_codename(self):
        self.assertEqual(identify_rocm_arch_from_name("Arcturus"), "gfx908")

    def test_cdna2_mi210(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Instinct MI210"), "gfx90a")

    def test_cdna2_mi250(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Instinct MI250"), "gfx90a")

    def test_cdna2_mi250x(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Instinct MI250X"), "gfx90a")

    def test_cdna2_bare_mi250x(self):
        # Must work without "AMD" prefix
        self.assertEqual(identify_rocm_arch_from_name("MI250X"), "gfx90a")

    def test_cdna2_codename_aldebaran(self):
        self.assertEqual(identify_rocm_arch_from_name("Aldebaran"), "gfx90a")

    def test_cdna2_mi200(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Instinct MI200"), "gfx90a")

    # -- RDNA name fallback --

    def test_rdna4_rx9070(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Radeon RX 9070 XT"), "gfx120X")

    def test_rdna3_rx7900(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Radeon RX 7900 XTX"), "gfx110X")

    def test_rdna2_rx6800(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Radeon RX 6800 XT"), "gfx103X")

    def test_strix_point_880m(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Radeon 890M"), "gfx1150")

    def test_strix_halo_8060s(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Radeon 8060S"), "gfx1151")

    def test_krackan_860m(self):
        self.assertEqual(identify_rocm_arch_from_name("AMD Radeon 860M"), "gfx1152")

    # -- Non-AMD names return empty --

    def test_nvidia_returns_empty(self):
        self.assertEqual(identify_rocm_arch_from_name("NVIDIA GeForce RTX 4090"), "")

    def test_intel_returns_empty(self):
        self.assertEqual(identify_rocm_arch_from_name("Intel Arc A770"), "")

    def test_empty_string(self):
        self.assertEqual(identify_rocm_arch_from_name(""), "")

    # -- Supported arch membership --

    def test_cdna_in_supported_set(self):
        for name in ("AMD Instinct MI100", "AMD Instinct MI250X", "90008", "90010"):
            arch = identify_rocm_arch_from_name(name)
            self.assertIn(arch, ROCM_SUPPORTED_FAMILIES, f"{name!r} -> {arch!r} not in supported families")


if __name__ == "__main__":
    unittest.main()
