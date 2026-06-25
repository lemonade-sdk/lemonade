#!/usr/bin/env python3
"""
CPU-runnable unit tests for ROCm specific-ISA -> release-target mapping logic
(system_info.cpp::rocm_arch_to_release_target()).

ROCm backend release repos (whisper.cpp-rocm, vllm-rocm, llamacpp-rocm nightly)
publish discrete RDNA GPUs under a *family* target (gfx103X / gfx110X / gfx120X)
while APUs ship per-specific ISA (gfx1150 / gfx1151 / gfx1152). get_rocm_arch()
returns the specific ISA (e.g. gfx1201), which is correct for TheRock runtime
paths but 404s on these per-target asset names. This test replicates
the C++ mapping so it can be validated without AMD hardware.

Run with: python -m pytest test/test_rocm_release_target.py
      or: python test/test_rocm_release_target.py
"""

import re
import unittest


# ---------------------------------------------------------------------------
# Python replica of system_info.cpp::rocm_arch_to_release_target()
# ---------------------------------------------------------------------------
def rocm_arch_to_release_target(arch: str) -> str:
    if not arch:
        return arch
    # Already a family target (trailing 'X') or non-gfx token: leave as-is.
    if arch.endswith("X") or not arch.startswith("gfx"):
        return arch
    # APUs are published per-specific ISA; do not collapse them.
    if arch in ("gfx1150", "gfx1151", "gfx1152"):
        return arch
    # 4-digit RDNA gfx token: collapse the trailing step nibble to 'X' to form
    # the family target for the families that publish family assets.
    if len(arch) == 7:
        base3 = arch[3:6]  # e.g. "120" from "gfx1201"
        if base3 in ("103", "110", "120"):
            return "gfx" + base3 + "X"
    # Anything else passes through unchanged.
    return arch


# ---------------------------------------------------------------------------
# Python replica of vllm_server.cpp ROCm release tag construction
# ---------------------------------------------------------------------------
def strip_vllm_rocm_target_suffix(version: str) -> str:
    # vllm-rocm GitHub releases are tagged as:
    #   vllm0.22.1-rocm7.13.0-gfx120X
    # Runtime config may contain a base version, or resolve_user_version() may
    # return a target-suffixed tag from GitHub latest / explicit user pin. Strip
    # the optional target suffix before appending this machine's target.
    return re.sub(r"-gfx(?:[0-9a-f]{3,4}|[0-9a-f]{3}X)$", "", version)


def vllm_rocm_release_tag(version: str, target_arch: str) -> str:
    return strip_vllm_rocm_target_suffix(version) + "-" + target_arch


class TestRocmReleaseTarget(unittest.TestCase):
    def test_rdna4_dgpu_collapses_to_family(self):
        # R9700 (gfx1201) must map to the published gfx120X target.
        self.assertEqual(rocm_arch_to_release_target("gfx1201"), "gfx120X")
        self.assertEqual(rocm_arch_to_release_target("gfx1200"), "gfx120X")

    def test_rdna3_dgpu_collapses_to_family(self):
        for isa in ("gfx1100", "gfx1101", "gfx1102", "gfx1103"):
            with self.subTest(isa=isa):
                self.assertEqual(rocm_arch_to_release_target(isa), "gfx110X")

    def test_rdna2_dgpu_collapses_to_family(self):
        for isa in ("gfx1030", "gfx1031", "gfx1032", "gfx1033",
                    "gfx1034", "gfx1035", "gfx1036"):
            with self.subTest(isa=isa):
                self.assertEqual(rocm_arch_to_release_target(isa), "gfx103X")

    def test_apus_stay_specific(self):
        # APU assets are published per-specific ISA; must NOT be collapsed.
        for isa in ("gfx1150", "gfx1151", "gfx1152"):
            with self.subTest(isa=isa):
                self.assertEqual(rocm_arch_to_release_target(isa), isa)

    def test_family_targets_are_idempotent(self):
        for fam in ("gfx103X", "gfx110X", "gfx120X"):
            with self.subTest(fam=fam):
                self.assertEqual(rocm_arch_to_release_target(fam), fam)

    def test_datacenter_and_other_archs_pass_through(self):
        # Data-center / CDNA and gfx101X dGPU have no family-collapse rule here.
        for isa in ("gfx908", "gfx90a", "gfx942", "gfx1010", "gfx1011", "gfx1012"):
            with self.subTest(isa=isa):
                self.assertEqual(rocm_arch_to_release_target(isa), isa)

    def test_empty_and_non_gfx_pass_through(self):
        for val in ("", "sm_120", "radeon", "unknown"):
            with self.subTest(val=val):
                self.assertEqual(rocm_arch_to_release_target(val), val)

    def test_maps_to_release_target_names_without_advertising_backend_support(self):
        # This mapping only chooses the asset-name token for recipes that already
        # decided the current GPU is supported. Recipe availability remains
        # controlled by RECIPE_DEFS in system_info.cpp (for example,
        # whispercpp/vllm intentionally do not advertise gfx103X or gfx1152).
        known_release_target_names = {"gfx103X", "gfx110X", "gfx120X",
                                      "gfx1150", "gfx1151", "gfx1152"}
        for isa in ("gfx1201", "gfx1100", "gfx1030", "gfx1151"):
            with self.subTest(isa=isa):
                self.assertIn(rocm_arch_to_release_target(isa), known_release_target_names)


class TestVllmRocmReleaseTags(unittest.TestCase):
    def test_base_version_appends_current_target(self):
        self.assertEqual(
            vllm_rocm_release_tag("vllm0.22.1-rocm7.13.0", "gfx120X"),
            "vllm0.22.1-rocm7.13.0-gfx120X",
        )

    def test_existing_target_suffix_is_replaced_not_double_appended(self):
        # GitHub marks one target release as "Latest". If resolve_user_version()
        # returns that full tag, vllm_server.cpp must rebuild the tag for the
        # current machine's ROCm release target rather than generating a bogus
        # "...-gfx1151-gfx120X" tag.
        self.assertEqual(
            vllm_rocm_release_tag("vllm0.22.1-rocm7.13.0-gfx1151", "gfx120X"),
            "vllm0.22.1-rocm7.13.0-gfx120X",
        )

    def test_existing_matching_target_suffix_is_idempotent(self):
        self.assertEqual(
            vllm_rocm_release_tag("vllm0.22.1-rocm7.13.0-gfx120X", "gfx120X"),
            "vllm0.22.1-rocm7.13.0-gfx120X",
        )

    def test_specific_target_suffix_can_be_replaced_with_specific_apu_target(self):
        self.assertEqual(
            vllm_rocm_release_tag("vllm0.22.1-rocm7.13.0-gfx120X", "gfx1151"),
            "vllm0.22.1-rocm7.13.0-gfx1151",
        )


if __name__ == "__main__":
    unittest.main()
