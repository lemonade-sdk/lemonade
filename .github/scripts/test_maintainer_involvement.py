#!/usr/bin/env python3
"""Regression tests for check_maintainer_involvement.py (stdlib only, no network).

Run with: python .github/scripts/test_maintainer_involvement.py
"""

import importlib.util
import sys
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_maintainer_involvement",
    Path(__file__).with_name("check_maintainer_involvement.py"),
)
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)


def requirements_for(paths, diff="", author="someone", approvers=()):
    return check.evaluate(
        check.build_requirements(paths, diff), author, {a.lower() for a in approvers}
    )


def unmet(requirements):
    return [r for r in requirements if not r["satisfied"]]


def required_sets(requirements):
    return {tuple(r["maintainers"]) for r in requirements}


def test_cli_change_requires_cli_maintainer():
    reqs = requirements_for(["src/cpp/cli/main.cpp"])
    assert required_sets(reqs) == {check.CLI_MAINTAINERS}
    assert unmet(reqs)

    assert not unmet(requirements_for(["src/cpp/cli/main.cpp"], approvers={"bitgamma"}))
    assert not unmet(requirements_for(["src/cpp/cli/main.cpp"], author="bitgamma"))


def test_app_change_requires_app_maintainer():
    reqs = requirements_for(["src/app/src/renderer/ChatWindow.tsx"])
    assert required_sets(reqs) == {check.APP_MAINTAINERS}
    assert not unmet(
        requirements_for(
            ["src/app/src/renderer/ChatWindow.tsx"], approvers={"kpoineal"}
        )
    )


def test_anything_else_requires_a_core_maintainer():
    reqs = requirements_for(["docs/README.md"])
    assert required_sets(reqs) == {check.CORE_MAINTAINERS}
    assert not unmet(requirements_for(["docs/README.md"], approvers={"ramkrishna2910"}))
    assert not unmet(requirements_for(["docs/README.md"], approvers={"jeremyfowers"}))


def test_scoped_paths_alone_do_not_pull_in_core_maintainers():
    reqs = requirements_for(["src/cpp/cli/main.cpp", "src/app/package.json"])
    assert required_sets(reqs) == {check.CLI_MAINTAINERS, check.APP_MAINTAINERS}


def test_mixed_change_stacks_every_rule():
    reqs = requirements_for(["src/cpp/cli/main.cpp", "src/cpp/server/router.cpp"])
    assert required_sets(reqs) == {check.CLI_MAINTAINERS, check.CORE_MAINTAINERS}

    # One maintainer's approval satisfies only their own rule.
    reqs = requirements_for(
        ["src/cpp/cli/main.cpp", "src/cpp/server/router.cpp"], approvers={"bitgamma"}
    )
    assert [r["maintainers"] for r in unmet(reqs)] == [check.CORE_MAINTAINERS]


def test_prefixes_must_match_a_directory_boundary():
    # src/appearance/ is not the desktop app.
    reqs = requirements_for(["src/appearance/theme.css"])
    assert required_sets(reqs) == {check.CORE_MAINTAINERS}


def test_network_keywords_add_a_second_reviewer():
    reqs = requirements_for(["docs/README.md"], diff="the CURL invocation")
    assert required_sets(reqs) == {check.CORE_MAINTAINERS, check.NETWORK_MAINTAINERS}

    # The extra reviewer stacks on top of, and never replaces, the area rule.
    reqs = requirements_for(["docs/README.md"], diff="tcp socket", approvers={"Geramy"})
    assert [r["maintainers"] for r in unmet(reqs)] == [check.CORE_MAINTAINERS]

    reqs = requirements_for(
        ["docs/README.md"], diff="tcp socket", approvers={"Geramy", "jeremyfowers"}
    )
    assert not unmet(reqs)


def test_rocm_keyword_adds_a_second_reviewer():
    reqs = requirements_for(["docs/README.md"], diff="build with ROCm 6.4")
    assert required_sets(reqs) == {check.CORE_MAINTAINERS, check.ROCM_MAINTAINERS}

    # Additive, like the networking rule — it never replaces the area rule.
    reqs = requirements_for(["docs/README.md"], diff="rocm", approvers={"superm1"})
    assert [r["maintainers"] for r in unmet(reqs)] == [check.CORE_MAINTAINERS]

    reqs = requirements_for(
        ["src/cpp/cli/main.cpp"], diff="rocm", approvers={"bitgamma", "superm1"}
    )
    assert not unmet(reqs)


def test_keyword_rules_stack_with_each_other():
    reqs = requirements_for(["docs/README.md"], diff="rocm over http")
    assert required_sets(reqs) == {
        check.CORE_MAINTAINERS,
        check.NETWORK_MAINTAINERS,
        check.ROCM_MAINTAINERS,
    }

    reqs = requirements_for(
        ["docs/README.md"], diff="rocm over http", approvers={"superm1", "jeremyfowers"}
    )
    assert [r["maintainers"] for r in unmet(reqs)] == [check.NETWORK_MAINTAINERS]


def test_network_keywords_match_case_insensitively_and_in_paths():
    assert check.NETWORK_MAINTAINERS in required_sets(
        requirements_for(["src/cpp/server/http_client.cpp"])
    )
    assert check.NETWORK_MAINTAINERS in required_sets(
        requirements_for(["docs/README.md"], diff="UDP beacon")
    )


def test_network_keywords_ignore_unchanged_context():
    reqs = requirements_for(["docs/README.md"], diff="no networking here")
    assert required_sets(reqs) == {check.CORE_MAINTAINERS}


def test_diff_text_reads_only_added_and_removed_lines():
    diff = "\n".join(
        [
            "diff --git a/f.md b/f.md",
            "--- a/f.md",
            "+++ b/f.md",
            "@@ -1,3 +1,3 @@",
            " context mentions curl",
            "-old line",
            "+new line",
        ]
    )

    original = check.gh_api
    check.gh_api = lambda *a, **k: diff
    try:
        text = check.diff_text(1, "o/r", [])
    finally:
        check.gh_api = original

    assert "old line" in text and "new line" in text
    assert "curl" not in text


def test_latest_review_verdict_wins():
    reviews = [
        {"user": {"login": "bitgamma"}, "state": "APPROVED"},
        {"user": {"login": "bitgamma"}, "state": "CHANGES_REQUESTED"},
    ]
    assert check.active_approvers(reviews) == set()

    reviews.append({"user": {"login": "bitgamma"}, "state": "APPROVED"})
    assert check.active_approvers(reviews) == {"bitgamma"}


def test_comments_do_not_displace_an_approval():
    reviews = [
        {"user": {"login": "Geramy"}, "state": "APPROVED"},
        {"user": {"login": "Geramy"}, "state": "COMMENTED"},
    ]
    assert check.active_approvers(reviews) == {"geramy"}


def test_dismissed_approval_does_not_count():
    reviews = [
        {"user": {"login": "kpoineal"}, "state": "APPROVED"},
        {"user": {"login": "kpoineal"}, "state": "DISMISSED"},
    ]
    assert check.active_approvers(reviews) == set()


def test_involvement_is_case_insensitive():
    assert not unmet(requirements_for(["docs/README.md"], author="JeremyFowers"))
    assert not unmet(requirements_for(["docs/README.md"], approvers={"RamKrishna2910"}))


def test_renames_count_both_sides():
    files = [{"filename": "src/app/b.ts", "previous_filename": "src/cpp/cli/a.cpp"}]
    assert check.changed_paths(files) == ["src/app/b.ts", "src/cpp/cli/a.cpp"]


def test_empty_pr_requires_nobody():
    assert check.build_requirements([], "") == []


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"PASS {name}")
        except AssertionError:
            failures += 1
            print(f"FAIL {name}")
            import traceback

            traceback.print_exc()
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
