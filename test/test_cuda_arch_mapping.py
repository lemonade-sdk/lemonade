#!/usr/bin/env python3
"""
CPU-runnable unit tests for CUDA compute capability -> sm_XX mapping logic.

These tests replicate the C++ compute_cap_to_sm() and
identify_cuda_arch_from_name() functions so the mapping can be validated
without NVIDIA hardware or a running server.

Run with: python -m pytest test/test_cuda_arch_mapping.py
      or: python test/test_cuda_arch_mapping.py
"""

import unittest

# ---------------------------------------------------------------------------
# Python replica of system_info.cpp::compute_cap_to_sm()
# ---------------------------------------------------------------------------

CUDA_SUPPORTED_ARCHS = {
    "sm_75",  # Turing
    "sm_80",  # Ampere DC
    "sm_86",  # Ampere
    "sm_89",  # Ada Lovelace
    "sm_90",  # Hopper
    "sm_100",  # Blackwell DC
    "sm_120",  # Blackwell consumer
}


def compute_cap_to_sm(compute_cap: str) -> str:
    """
    Convert a "MAJOR.MINOR" compute-capability string from nvidia-smi to the
    sm_XX token used in llamacpp-cuda release asset filenames.
    Returns "" if the value cannot be parsed.
    """
    dot = compute_cap.find(".")
    if dot == -1:
        return ""
    major, minor = compute_cap[:dot], compute_cap[dot + 1 :]
    if not major or not minor:
        return ""
    try:
        return f"sm_{int(major) * 10 + int(minor)}"
    except ValueError:
        return ""


# ---------------------------------------------------------------------------
# Python replica of system_info.cpp::identify_cuda_arch_from_name() (compact)
# ---------------------------------------------------------------------------

_NAME_ARCH_TABLE = [
    # Blackwell DC (sm_100)
    (["b100", "b200"], "sm_100"),
    # Hopper (sm_90)
    (["h100", "h200"], "sm_90"),
    # Ampere DC (sm_80)
    (["a100"], "sm_80"),
    # Blackwell consumer (sm_120)
    (["rtx 50", "rtx50", "5090", "5080", "5070", "5060"], "sm_120"),
    # Ada Lovelace (sm_89)
    (["rtx 40", "rtx40", "4090", "4080", "4070", "4060", "l40", " l4"], "sm_89"),
    # Ampere consumer/pro (sm_86)
    (
        [
            "rtx 30",
            "rtx30",
            "3090",
            "3080",
            "3070",
            "3060",
            "3050",
            "a40",
            "a30",
            "a10",
            "a6000",
            "a5000",
            "a4000",
            "a2000",
        ],
        "sm_86",
    ),
    # Turing (sm_75)
    (
        [
            "rtx 20",
            "rtx20",
            "2080",
            "2070",
            "2060",
            "gtx 16",
            "gtx16",
            "1660",
            "1650",
            "titan rtx",
            "quadro rtx",
            " t4",
        ],
        "sm_75",
    ),
]


def identify_cuda_arch_from_name(device_name: str) -> str:
    """Compact fallback: infer sm_XX from GPU marketing name."""
    name = device_name.lower()
    # Require at least one NVIDIA identifier
    nvidia_ids = [
        "nvidia",
        "geforce",
        "rtx",
        "gtx",
        "quadro",
        "tesla",
        "titan",
        "a100",
        "a40",
        "a30",
        "a10",
        "h100",
        "h200",
        "b100",
        "b200",
        "l40",
        "l4",
    ]
    if not any(kw in name for kw in nvidia_ids):
        return ""
    for keywords, sm in _NAME_ARCH_TABLE:
        if any(kw in name for kw in keywords):
            return sm
    return ""


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestComputeCapToSm(unittest.TestCase):
    def test_valid_mappings(self):
        cases = [
            ("7.5", "sm_75"),
            ("8.0", "sm_80"),
            ("8.6", "sm_86"),
            ("8.9", "sm_89"),
            ("9.0", "sm_90"),
            ("10.0", "sm_100"),
            ("12.0", "sm_120"),
        ]
        for cap, expected in cases:
            with self.subTest(cap=cap):
                self.assertEqual(compute_cap_to_sm(cap), expected)

    def test_unsupported_but_parseable(self):
        # Should still parse even if not in CUDA_SUPPORTED_ARCHS
        self.assertEqual(compute_cap_to_sm("6.1"), "sm_61")
        self.assertEqual(compute_cap_to_sm("5.0"), "sm_50")

    def test_invalid_inputs(self):
        for bad in ["", "86", "8", ".", "abc", "8.x"]:
            with self.subTest(bad=bad):
                self.assertEqual(compute_cap_to_sm(bad), "")

    def test_supported_arch_membership(self):
        for cap in ["7.5", "8.0", "8.6", "8.9", "9.0", "10.0", "12.0"]:
            sm = compute_cap_to_sm(cap)
            self.assertIn(
                sm, CUDA_SUPPORTED_ARCHS, f"{cap} -> {sm} not in supported set"
            )

    def test_unsupported_arch_not_in_set(self):
        for cap in ["6.1", "7.0", "5.0"]:
            sm = compute_cap_to_sm(cap)
            self.assertNotIn(
                sm, CUDA_SUPPORTED_ARCHS, f"{cap} -> {sm} unexpectedly in supported set"
            )


class TestIdentifyCudaArchFromName(unittest.TestCase):
    def test_known_consumer_gpus(self):
        cases = [
            ("NVIDIA GeForce RTX 3080", "sm_86"),
            ("NVIDIA GeForce RTX 4090", "sm_89"),
            ("NVIDIA GeForce RTX 2080 Ti", "sm_75"),
            ("NVIDIA GeForce RTX 5090", "sm_120"),
            ("NVIDIA GTX 1660 Super", "sm_75"),
            ("NVIDIA H100 PCIe", "sm_90"),
            ("NVIDIA A100-SXM4-80GB", "sm_80"),
            ("NVIDIA A10", "sm_86"),
            ("NVIDIA A40", "sm_86"),
        ]
        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(identify_cuda_arch_from_name(name), expected)

    def test_non_nvidia_returns_empty(self):
        for name in ["AMD Radeon RX 7900 XTX", "Intel Arc A770", "Apple M3", ""]:
            with self.subTest(name=name):
                self.assertEqual(identify_cuda_arch_from_name(name), "")

    def test_unsupported_nvidia_returns_empty(self):
        # Kepler/Maxwell/Pascal — not in CUDA_SUPPORTED_ARCHS
        for name in ["NVIDIA GTX 1080 Ti", "NVIDIA GTX 980", "NVIDIA Tesla K80"]:
            with self.subTest(name=name):
                result = identify_cuda_arch_from_name(name)
                # May return an sm_XX token but it won't be in the supported set
                if result:
                    self.assertNotIn(result, CUDA_SUPPORTED_ARCHS)


if __name__ == "__main__":
    unittest.main()
