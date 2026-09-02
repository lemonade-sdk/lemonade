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

CLI = check.VERTICALS[0]["reviewers"]
GUI = check.VERTICALS[1]["reviewers"]
EVERYTHING_ELSE = check.FALLBACK_VERTICAL["reviewers"]
NETWORKING = check.HORIZONTALS[0]["reviewers"]
ROCM = check.HORIZONTALS[1]["reviewers"]


def reviews_for(paths, diff="", body="", author="someone", approvers=()):
    return check.evaluate(
        check.required_reviews(paths, diff, body),
        author,
        {a.lower() for a in approvers},
    )


def trigger_for(required, area):
    return next(r["trigger"] for r in required if r["area"] == area)


def unmet(required):
    return [r for r in required if not r["satisfied"]]


def reviewer_sets(required):
    return {tuple(r["reviewers"]) for r in required}


def roles(required):
    return {r["area"]: r["role"] for r in required}


def test_cli_vertical_needs_its_primary_reviewer():
    required = reviews_for(["src/cpp/cli/main.cpp"])
    assert reviewer_sets(required) == {CLI}
    assert roles(required) == {"CLI": "primary"}
    assert unmet(required)

    assert not unmet(reviews_for(["src/cpp/cli/main.cpp"], approvers={"bitgamma"}))
    assert not unmet(reviews_for(["src/cpp/cli/main.cpp"], author="bitgamma"))


def test_gui_vertical_needs_its_primary_reviewer():
    required = reviews_for(["src/app/src/renderer/ChatWindow.tsx"])
    assert reviewer_sets(required) == {GUI}
    assert not unmet(
        reviews_for(["src/app/src/renderer/ChatWindow.tsx"], approvers={"kpoineal"})
    )


def test_everything_else_falls_to_the_project_maintainers():
    required = reviews_for(["docs/README.md"])
    assert reviewer_sets(required) == {EVERYTHING_ELSE}
    assert roles(required) == {"Everything else": "primary"}
    assert not unmet(reviews_for(["docs/README.md"], approvers={"ramkrishna2910"}))
    assert not unmet(reviews_for(["docs/README.md"], approvers={"jeremyfowers"}))


def test_owned_verticals_do_not_leak_into_everything_else():
    required = reviews_for(["src/cpp/cli/main.cpp", "src/app/package.json"])
    assert reviewer_sets(required) == {CLI, GUI}


def test_every_vertical_a_pr_touches_needs_its_own_primary_review():
    required = reviews_for(["src/cpp/cli/main.cpp", "src/cpp/server/router.cpp"])
    assert reviewer_sets(required) == {CLI, EVERYTHING_ELSE}

    # One primary reviewer's approval satisfies only their own vertical.
    required = reviews_for(
        ["src/cpp/cli/main.cpp", "src/cpp/server/router.cpp"], approvers={"bitgamma"}
    )
    assert [r["reviewers"] for r in unmet(required)] == [EVERYTHING_ELSE]


def test_prefixes_must_match_a_directory_boundary():
    # src/appearance/ is not the GUI.
    assert reviewer_sets(reviews_for(["src/appearance/theme.css"])) == {EVERYTHING_ELSE}


NETWORKING_AREA = check.HORIZONTALS[0]["name"]


def test_networking_horizontal_adds_an_expert_review():
    required = reviews_for(["docs/README.md"], diff="the CURL invocation")
    assert reviewer_sets(required) == {EVERYTHING_ELSE, NETWORKING}
    assert roles(required)[NETWORKING_AREA] == "expert"


def test_security_is_a_networking_horizontal_keyword():
    assert NETWORKING in reviewer_sets(
        reviews_for(["docs/README.md"], diff="tighten the Security check")
    )


def test_horizontals_also_read_the_pr_body():
    required = reviews_for(["docs/README.md"], body="This is a security fix.")
    assert reviewer_sets(required) == {EVERYTHING_ELSE, NETWORKING}
    assert trigger_for(required, NETWORKING_AREA) == "PR body mentions security"

    required = reviews_for(["docs/README.md"], body="Rebuild against ROCm.")
    assert reviewer_sets(required) == {EVERYTHING_ELSE, ROCM}


def test_trigger_names_where_the_keyword_was_found():
    required = reviews_for(["docs/README.md"], diff="http", body="unrelated")
    assert trigger_for(required, NETWORKING_AREA) == "diff mentions http"

    required = reviews_for(["docs/README.md"], diff="http", body="cors matters")
    assert (
        trigger_for(required, NETWORKING_AREA) == "diff and PR body mention cors, http"
    )


def test_rocm_horizontal_adds_an_expert_review():
    required = reviews_for(["docs/README.md"], diff="build with ROCm 6.4")
    assert reviewer_sets(required) == {EVERYTHING_ELSE, ROCM}
    assert roles(required)["ROCm"] == "expert"


def test_an_expert_review_never_replaces_the_primary_one():
    required = reviews_for(["docs/README.md"], diff="tcp socket", approvers={"Geramy"})
    assert [r["reviewers"] for r in unmet(required)] == [EVERYTHING_ELSE]

    required = reviews_for(["docs/README.md"], diff="rocm", approvers={"superm1"})
    assert [r["reviewers"] for r in unmet(required)] == [EVERYTHING_ELSE]

    required = reviews_for(
        ["src/cpp/cli/main.cpp"], diff="rocm", approvers={"bitgamma", "superm1"}
    )
    assert not unmet(required)


def test_horizontals_stack_with_each_other():
    required = reviews_for(["docs/README.md"], diff="rocm over http")
    assert reviewer_sets(required) == {EVERYTHING_ELSE, NETWORKING, ROCM}

    required = reviews_for(
        ["docs/README.md"], diff="rocm over http", approvers={"superm1", "jeremyfowers"}
    )
    assert [r["reviewers"] for r in unmet(required)] == [NETWORKING]


def test_horizontal_keywords_match_case_insensitively_and_in_paths():
    assert NETWORKING in reviewer_sets(reviews_for(["src/cpp/server/http_client.cpp"]))
    assert ROCM in reviewer_sets(reviews_for(["src/cpp/rocm_device.cpp"]))
    assert NETWORKING in reviewer_sets(
        reviews_for(["docs/README.md"], diff="UDP beacon")
    )


def test_horizontals_ignore_unchanged_context():
    assert reviewer_sets(reviews_for(["docs/README.md"], diff="nothing relevant")) == {
        EVERYTHING_ELSE
    }


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
        {"user": {"login": "bitgamma"}, "state": "APPROVED", "commit_id": "head"},
        {
            "user": {"login": "bitgamma"},
            "state": "CHANGES_REQUESTED",
            "commit_id": "head",
        },
    ]
    assert check.active_approvers(reviews, "head") == set()

    reviews.append(
        {"user": {"login": "bitgamma"}, "state": "APPROVED", "commit_id": "head"}
    )
    assert check.active_approvers(reviews, "head") == {"bitgamma"}


def test_a_push_after_an_approval_invalidates_it():
    reviews = [{"user": {"login": "bitgamma"}, "state": "APPROVED", "commit_id": "old"}]
    assert check.active_approvers(reviews, "old") == {"bitgamma"}
    assert check.active_approvers(reviews, "new") == set()

    reviews.append(
        {"user": {"login": "bitgamma"}, "state": "APPROVED", "commit_id": "new"}
    )
    assert check.active_approvers(reviews, "new") == {"bitgamma"}


def test_comments_do_not_displace_an_approval():
    reviews = [
        {"user": {"login": "Geramy"}, "state": "APPROVED", "commit_id": "head"},
        {"user": {"login": "Geramy"}, "state": "COMMENTED", "commit_id": "head"},
    ]
    assert check.active_approvers(reviews, "head") == {"geramy"}


def test_a_comment_alone_is_not_an_approval():
    reviews = [{"user": {"login": "Geramy"}, "state": "COMMENTED", "commit_id": "head"}]
    assert check.active_approvers(reviews, "head") == set()


def test_dismissed_approval_does_not_count():
    reviews = [
        {"user": {"login": "kpoineal"}, "state": "APPROVED", "commit_id": "head"},
        {"user": {"login": "kpoineal"}, "state": "DISMISSED", "commit_id": "head"},
    ]
    assert check.active_approvers(reviews, "head") == set()


def test_involvement_is_case_insensitive():
    assert not unmet(reviews_for(["docs/README.md"], author="JeremyFowers"))
    assert not unmet(reviews_for(["docs/README.md"], approvers={"RamKrishna2910"}))


def test_renames_count_both_sides():
    files = [{"filename": "src/app/b.ts", "previous_filename": "src/cpp/cli/a.cpp"}]
    assert check.changed_paths(files) == ["src/app/b.ts", "src/cpp/cli/a.cpp"]


def test_every_non_empty_pr_requires_a_primary_review():
    """The verticals are exhaustive: no set of paths escapes a primary review."""
    for paths in (
        ["src/cpp/cli/a.cpp"],
        ["src/app/a.ts"],
        ["README.md"],
        ["src/app/a.ts", "src/cpp/cli/a.cpp", "README.md"],
    ):
        assert any(r["role"] == "primary" for r in reviews_for(paths))


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
