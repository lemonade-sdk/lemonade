"""
Two pre-existing Lemonade bugs (unrelated to the VTE integration itself)
were found and fixed WHILE validating the integration for real, by
compiling and running lemond.exe against real hardware (RX 7600):

1. `/MANIFESTINPUT:` without quotes in CMakeLists.txt breaks the link step
   (LNK1181) whenever the repo sits at a path containing a space (e.g. this
   machine, "Aetheris Flow").
2. `identify_rocm_arch_from_name()` never recognized "7600" as RDNA3 -- the
   RX 7600 itself (the card the whole VTE project was built and measured
   on) never showed up as supported on ANY ROCm backend (not llamacpp, not
   vte), purely because of this gap in name-based detection.

Both confirmed fixed on a real build (cmake --build --preset windows) and
against a real running lemond.exe (the RX 7600 started showing up as
"gfx110X" in /api/v1/system-info, and vte/llamacpp both went from
"unsupported" to "installable").

Delete this folder once the integration is complete and validated.
"""
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKE = REPO / "CMakeLists.txt"
SYSTEM_INFO_CPP = REPO / "src/cpp/server/system_info.cpp"


def test_manifest_input_path_is_quoted():
    text = CMAKE.read_text(encoding="utf-8")
    assert '/MANIFESTINPUT:\\"${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest\\"' in text, (
        "The /MANIFESTINPUT: flag needs the path wrapped in escaped quotes -- "
        "without it, linking lemond.exe breaks (LNK1181) on any checkout "
        "whose path contains a space (confirmed on a real build this session)."
    )


def test_rx7600_recognized_as_gfx110x():
    text = SYSTEM_INFO_CPP.read_text(encoding="utf-8")
    # The same condition that already recognizes 7700/7800/7900/v710 as
    # gfx110X also needs to check "7600" -- without it, the RX 7600 never
    # shows up as supported RDNA3 on ANY ROCm backend (confirmed by running
    # a real lemond.exe: GPU family came back "" before the fix, "gfx110X"
    # after).
    idx = text.find('return "gfx110X"')
    assert idx != -1, "gfx110X detection block not found in system_info.cpp"
    preceding = text[max(0, idx - 400):idx]
    assert '"7600"' in preceding, (
        "'7600' needs to appear in the chain of checks preceding 'return \"gfx110X\"' "
        "-- without it, the RX 7600 is never recognized as RDNA3 by name."
    )
