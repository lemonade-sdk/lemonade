#!/usr/bin/env python3
"""Regression tests for triage_dashboard.py (stdlib only, no network).

Run with: python .github/scripts/test_triage_dashboard.py
"""

import importlib.util
import json
import sys
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "triage_dashboard", Path(__file__).with_name("triage_dashboard.py")
)
triage_dashboard = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(triage_dashboard)


def test_script_safe_json_neutralizes_script_breakout():
    """A title that tries to close the <script> tag must not survive verbatim."""
    payload = "</script><script>alert(1)</script>"
    serialized = triage_dashboard.script_safe_json([{"title": payload}])

    # The raw breakout sequence must not appear; < > & are unicode-escaped.
    assert "</script>" not in serialized
    assert "<script>" not in serialized
    assert "\\u003c" in serialized and "\\u003e" in serialized

    # It must still be valid JSON that round-trips to the original value.
    assert json.loads(serialized) == [{"title": payload}]


def test_script_safe_json_escapes_line_separators():
    """U+2028 / U+2029 are legal in JSON strings but break JS string literals."""
    serialized = triage_dashboard.script_safe_json(["a b c"])
    assert " " not in serialized
    assert " " not in serialized
    assert "\\u2028" in serialized and "\\u2029" in serialized
    assert json.loads(serialized) == ["a b c"]


def test_generated_html_contains_no_script_breakout():
    """End-to-end: a malicious title embedded in the template stays inert."""
    items = [{"number": 1, "title": "</script><script>alert(1)</script>", "labels": []}]
    html = triage_dashboard.HTML_TEMPLATE.replace(
        "__DATA__", triage_dashboard.script_safe_json(items)
    )
    # Only the template's own trailing </script> should remain — exactly one.
    assert html.count("</script>") == 1


def _run():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for test in tests:
        test()
        print(f"ok - {test.__name__}")
    print(f"\n{len(tests)} passed")


if __name__ == "__main__":
    _run()
    sys.exit(0)
