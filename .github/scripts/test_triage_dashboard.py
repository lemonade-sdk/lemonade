#!/usr/bin/env python3
"""Regression tests for triage_dashboard.py (stdlib only, no network).

Run with: python .github/scripts/test_triage_dashboard.py
"""

import importlib.util
import json
import sys
import unittest.mock as mock
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

    # The payload's < > characters must be escaped — the key XSS defences.
    assert "\\u003c/script\\u003e" in serialized


def test_script_safe_json_escapes_line_separators():
    """U+2028 / U+2029 are legal in JSON strings but break JS string literals."""
    ls = " "
    ps = " "
    serialized = triage_dashboard.script_safe_json([f"a{ls}b{ps}c"])
    assert ls not in serialized
    assert ps not in serialized
    assert "\\u2028" in serialized and "\\u2029" in serialized


def test_generated_html_contains_no_script_breakout():
    """End-to-end: a malicious title embedded in the template stays inert."""
    items = [{"number": 1, "title": "</script><script>alert(1)</script>", "labels": []}]
    html = triage_dashboard.HTML_TEMPLATE.replace(
        "__DATA__", triage_dashboard.script_safe_json(items)
    ).replace("__RFC_DATA__", triage_dashboard.script_safe_json([]))
    # Only the template's own trailing </script> should remain — exactly one.
    assert html.count("</script>") == 1


def test_parse_wg_from_template_checkbox():
    """Regex pattern matches PR-template checkbox line."""
    body = "- [x] is within the scope of WG: Auto-Tune\n## Summary"
    assert triage_dashboard.parse_wg(body) == "Auto-Tune"

    body2 = "- [x] is within the scope of WG: Cross-Vendor Support"
    assert triage_dashboard.parse_wg(body2) == "Cross-Vendor Support"

    body3 = "within the scope of WG: Enterprise Grade"
    assert triage_dashboard.parse_wg(body3) == "Enterprise Grade"


def test_parse_wg_from_body_scan_fallback():
    """Prose mention without the template line is caught by body scan."""
    body = "This improves the Auto-Tune system for better model selection."
    assert triage_dashboard.parse_wg(body) == "Auto-Tune"

    # Longer match wins over shorter prefix
    body2 = "Relates to Cross-Vendor Support initiative for NVIDIA."
    assert triage_dashboard.parse_wg(body2) == "Cross-Vendor Support"

    # No WG mention
    assert triage_dashboard.parse_wg("Fixes a CLI bug with no WG.") is None

    # None and empty
    assert triage_dashboard.parse_wg(None) is None
    assert triage_dashboard.parse_wg("") is None


def test_staleness_bucket():
    """Bucket boundaries are correct at all thresholds."""
    assert triage_dashboard.staleness_bucket(0) == "active"
    assert triage_dashboard.staleness_bucket(13) == "active"
    assert triage_dashboard.staleness_bucket(14) == "stale::14d"
    assert triage_dashboard.staleness_bucket(29) == "stale::14d"
    assert triage_dashboard.staleness_bucket(30) == "stale::30d"
    assert triage_dashboard.staleness_bucket(59) == "stale::30d"
    assert triage_dashboard.staleness_bucket(60) == "stale::60d+"
    assert triage_dashboard.staleness_bucket(365) == "stale::60d+"


def test_contributor_tier():
    """All known author_association values map to the right tier."""
    assert triage_dashboard.contributor_tier("OWNER") == "maintainer"
    assert triage_dashboard.contributor_tier("MEMBER") == "maintainer"
    assert triage_dashboard.contributor_tier("COLLABORATOR") == "maintainer"
    assert triage_dashboard.contributor_tier("FIRST_TIME_CONTRIBUTOR") == "first-timer"
    assert triage_dashboard.contributor_tier("CONTRIBUTOR") == "returning"
    assert triage_dashboard.contributor_tier("NONE") == "returning"
    assert triage_dashboard.contributor_tier("") == "returning"


def test_compute_priority_score_ordering():
    """Hot > warm; stale+unassigned+first-timer > plain warm."""
    hot_pr = {
        "labels": ["priority::🔥hot"],
        "days_stale": 0,
        "assignees": ["bob"],
        "contributor_tier": "returning",
        "comments": 0,
    }
    warm_pr = {
        "labels": ["priority::😎warm"],
        "days_stale": 0,
        "assignees": ["bob"],
        "contributor_tier": "returning",
        "comments": 0,
    }
    stale_unassigned_firsttimer = {
        "labels": [],
        "days_stale": 65,
        "assignees": [],
        "contributor_tier": "first-timer",
        "comments": 0,
    }

    hot_score = triage_dashboard.compute_priority_score(hot_pr)
    warm_score = triage_dashboard.compute_priority_score(warm_pr)
    stale_score = triage_dashboard.compute_priority_score(stale_unassigned_firsttimer)

    assert hot_score > warm_score, "hot must outrank warm"
    assert (
        stale_score > warm_score
    ), "stale+unassigned+first-timer must outrank plain warm"
    assert hot_score > stale_score, "hot must outrank stale+unassigned+first-timer"


def test_fetch_rfcs_graceful_failure():
    """fetch_rfcs must return [] without raising if the API call fails."""
    with mock.patch.object(
        triage_dashboard, "gh_api_pages", side_effect=Exception("network error")
    ):
        result = triage_dashboard.fetch_rfcs("some/repo")
    assert result == []


def test_rfc_data_xss_safe():
    """XSS attempt in RFC title is neutralised by script_safe_json."""
    # script_safe_json is designed for JS embedding, not Python json.loads
    # (it escapes U+2028/U+2029 and space which breaks json.loads).
    # Verify the security property: the injection string is defused.
    payload = "</script><script>alert(1)</script>"
    serialized = triage_dashboard.script_safe_json([{"title": payload}])
    assert "</script>" not in serialized
    assert "\\u003c" in serialized and "\\u003e" in serialized


def test_generated_html_has_rfc_placeholder():
    """HTML_TEMPLATE must contain __RFC_DATA__ for the RFC section to work."""
    assert "__RFC_DATA__" in triage_dashboard.HTML_TEMPLATE


def _run():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for test in tests:
        test()
        print(f"ok - {test.__name__}")
    print(f"\n{len(tests)} passed")


if __name__ == "__main__":
    _run()
    sys.exit(0)
