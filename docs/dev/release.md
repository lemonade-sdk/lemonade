# Lemonade Release Process

This guide documents the end-to-end process of releasing Lemonade.

## Quality

Lemonade has built its brand on quality and ease-of-use. Do not release a new Lemonade version if this is compromised in any way.

The repo-manager workflow runs automatically on every push to `main` or a release branch and publishes a live release dashboard at [https://lemonade-server.ai/repo-manager](https://lemonade-server.ai/repo-manager). Use it to assess whether the release is ready to ship. It also maintains three GitHub issues for the upcoming release (described in the steps below); these are updated automatically on each push.

## Release Cadence and Channels

Lemonade operates on a weekly release cadence. A release candidate is branched from `main` every Wednesday at 19:00 UTC and tested by the community for the following week. A release admin decides whether to promote a tested candidate to the stable channels.

Release candidates are published for Ubuntu, Windows, Docker, macOS, Fedora, and Debian. The PPA and Snap use the `candidate` channel, and GitHub marks the release as a prerelease. Builds from `main` continue to go to the `bleeding-edge` channels. Stable releases use the `stable` PPA and Snap channels and the `latest` GitHub and Docker channels.

Any PR intended for the next release should be merged before the Wednesday cutoff. A commit merged after 19:00 UTC belongs to the following release cycle. Maintainers may postpone merging a PR to protect release quality.

## Versioning

Lemonade versions have the deterministic format `year.week.number`:

- `year` is the four-digit year of the release.
- `week` is the ISO week in which the stable release is scheduled.
- `number` is the number of commits added to the release branch since it was created. It starts at `0` and increases monotonically. Each number identifies exactly one artifact, whether or not that artifact is promoted to stable.

Release branches omit the final component and are named `release-v<year>.<week>`. For example, a branch created on Wednesday, September 9, 2026 for the following week's release is named `release-v2026.38`. Its first candidate is `v2026.38.0`; one fix on the release branch produces `v2026.38.1`. If `v2026.38.1` is promoted to stable and later needs a hotfix, the next artifact is `v2026.38.2`.

Windows MSI versions use a two-digit year because MSI version fields do not support the four-digit value. For example, Lemonade `2026.38.1` has MSI version `26.38.1`.

The version is calculated at build time rather than stored in `CMakeLists.txt`. Non-candidate builds, including source builds, PR checks, and `bleeding-edge` artifacts, use:

```text
year.week.0~<commit-count>.<short-hash>
```

The week is the release week the commit would enter. Before the Wednesday 19:00 UTC cutoff, this is the following week; after the cutoff, it is the week after that. For example, a build from `main` on September 10, 2026 could be `2026.39.0~1595.ff22950d`.

A source tree may contain a `.version` file to override the calculated version. Release source archives include this file so unpacked source retains the version of the artifact from which it came.

## Release Lifecycle

### 1. Create and Publish the First Candidate

At 19:00 UTC every Wednesday, automation creates `release-v<year>.<week>` from the tip of `main`, where the year and week identify the following week's release. The initial branch build is candidate `.0`.

Every push to a `release-v*` branch produces the next candidate and publishes it to the candidate/prerelease channels. Creating the branch also causes repo-manager to create three GitHub issues:

- **`Release v<year>.<week> final checklist`** — a prioritized P1/P2/P3 checklist and a machine-generated `Ready` or `Needs Attention` verdict.
- **`v<year>.<week> release notes`** — pre-populated Headline and Breaking Changes sections used by the release action.
- **`v<year>.<week> announcement`** — a Discord announcement draft with feature sections and contributor shoutouts.

Repo-manager re-syncs the issues after every push to the release branch. Comments on those issues are considered when it regenerates their content.

### 2. Test the Candidate

The release admin moderates the `#release-candidate` Discord channel, announces each candidate, and uses the repo-manager checklist to tell volunteers what needs testing. The admin also monitors issues with the `release-candidate` label and triages them against the project's supported use cases and quality standards.

At minimum, each platform needs someone to install or upgrade to the candidate and exercise it. Testing normal production workloads is encouraged. Candidate testers should back up their configuration and models before installing a candidate, especially when a release includes a schema migration or another breaking change.

When testing finds a problem, choose one of these outcomes:

1. **Fix in a future release:** If the problem is minor and non-breaking, record it and fix it on `main` for a later release.
2. **Hotfix:** Merge a minimal fix into `main`, then cherry-pick that commit into the release branch. Never develop a fix only on the release branch.
3. **Revert:** If a blocking problem would be too complex to fix safely during the release cycle, revert the responsible commit or commits from both `main` and the release branch. The change must return to PR review before it can land again.

Pushing a hotfix or revert to the release branch automatically publishes a new candidate with an incremented `number`. If an automated cherry-pick conflicts, stop and resolve the situation through the normal reviewed development process; do not introduce release-only code.

Workflow-dispatch tools are available to perform the common branch operations:

- **Cherry-pick to release branch:** cherry-picks one or more fixes from `main` into the current release branch.
- **Release hotfix:** cherry-picks one or more commits into an older `release-v*` branch, causing a new candidate to be published before it is considered for stable.
- **Revert commit:** reverts one or more commits from both `main` and the current release branch.

When testing exposes a missing automated test, file an issue or RFC to add that coverage so the same class of regression is caught in CI.

### 3. Decide Whether to Release

Stable publication is always a human decision. Before tagging a candidate:

- Check the **`Release v<year>.<week> final checklist`** issue. Resolve all P1 items and confirm that its verdict is `Ready`.
- Review the candidate feedback and all open issues carrying the `release-candidate` label.
- Review and edit the release-notes issue as described below.

If blocking issues remain at 19:00 UTC on Friday, skip that week's stable release by leaving the release branch untagged. Hold a postmortem to decide whether the review, testing, or release policy needs adjustment. Candidate artifacts keep their existing versions; version numbers are never reused.

### 4. Review the Release Notes

Open the **`v<year>.<week> release notes`** GitHub issue. Repo-manager pre-populates the **Headline** and **Breaking Changes** sections from the commit history. Review and edit them before tagging because the release action pulls these sections directly into the GitHub release page.

#### Headline

The headline section starts with `## Headline` and contains a single-depth bulleted list. The website at https://lemonade-server.ai parses this section, so preserve that format.

List the three to five most noteworthy aspects of the release. Keep each item concise, with no special formatting, links, or shoutouts.

#### Breaking Changes

Use a bulleted list with one item per breaking change. Keep it concise and link to a wiki article when users need migration instructions.

### 5. Promote a Candidate to Stable

Tag the exact commit that produced the tested candidate. The tag must be `v<year>.<week>.<number>` and must match that candidate's build version. Pushing the tag is the human gate that starts stable publication; do not create release tags for candidates that should remain only in the candidate/prerelease channels.

Use `tools/release.py` to do this deterministically. It fetches the release branches, selects the newest `release-v*` branch, computes the version from that branch's tip with the same logic as the build (`tools/version.py`), and — after you confirm — creates a signed tag on that exact commit and pushes it. It refuses to proceed if the computed tag already exists or if the branch advanced between selection and push, which prevents accidentally tagging the wrong `number`.

```bash
# Promote the newest active release branch
python tools/release.py

# Promote an explicit older branch (hotfix)
python tools/release.py release-v2026.34

# Preview the tag it would create without pushing anything
python tools/release.py --dry-run
```

It prints the selected branch, commit, and computed version, then prompts `Create and push v<year>.<week>.<number>?` before tagging. Because the version is derived from the branch, you cannot promote a candidate by typing the wrong number.

The [cpp_server_build_test_release.yml workflow](https://github.com/lemonade-sdk/lemonade/blob/main/.github/workflows/cpp_server_build_test_release.yml) creates the stable GitHub release and publishes the stable artifacts.

### 6. Approve Windows Signing

Lemonade .msi artifacts are signed by SignPath.io under their SignPath Foundation program. Thank you SignPath!

Every release must be manually approved on SignPath. After the Windows installer is built by the release action, a job called Sign MSI Installers with SignPath will start.

Example from v10.7.0: https://github.com/lemonade-sdk/lemonade/actions/runs/27283434473/job/80587971984

The log will include a link like this:

```
You can view the signing request here: https://app.signpath.io/Web/8103545b-7814-4edc-86d6-a91dc2a2291b/SigningRequests/7eab3b1c-0ad2-4a65-93a5-683a8065e926
```

You must click the link, sign in, and press `Approve`.

### 7. Wait for the Release Action

Wait for the `cpp_server_build_test_release.yml` action to complete successfully. If a failure is transient, rerun the failed job. If code needs to change, fix it on `main` and use the hotfix process to create and test a new candidate. Do not move or force-push an existing release tag: each version identifies exactly one artifact.

After publication, you may need to edit the GitHub release to:
- add co-author contributors who were missing
- add deprecation notices
- add/remove links to release artifacts that we forgot to update in the release workflow.

DO NOT add or replace release artifacts, as this would break the chain of custody for the release and damage our credibility with users.

### 8. Post the Discord Announcement

Open the **`v<year>.<week> announcement`** GitHub issue. Repo-manager has drafted a full announcement with per-feature sections and contributor shoutouts. Review it, make any edits, and post it in `#announcements` on the Lemonade Discord.

Use `@everyone` for the regular weekly release and `@release` for a hotfix.

The auto-generated announcement uses people's GitHub usernames for shoutouts. Translate those to Discord usernames to the best of your ability before posting.

## Social Media

Not required, but it is always good to promote the new release online.

### Reddit

There are two kinds of Reddit posts we typically do. It's okay to do both for a single release, but in that case publish one on release day and the other at least two days later.

**Release Update**

Covers the entire release. This can be similar to the announcement posted on Discord, but:
- Be sure to remove Discord-specific artifacts like `@everyone`.
- Add context for people who are not Lemonade users.
- Add call-to-action (CTA) content to the bottom. Typically a link to the GitHub, a link to the Discord, and some request such as giving feedback on a plan or trying something out.

Example of a successful Reddit release updates:
- Major update: https://www.reddit.com/r/LocalLLaMA/comments/1rsucvk/lemonade_v10_linux_npu_support_and_chock_full_of/
- Minor update: https://www.reddit.com/r/LocalLLaMA/comments/1u26wkb/lemonade_v107_release_and_project_organization/

**Feature Update**

Covers a specific feature in-depth. Always include a graphical asset to draw people in.

Examples of successful Reddit feature posts:
- https://www.reddit.com/r/LocalLLaMA/comments/1t7g70j/vllm_rocm_has_been_added_to_lemonade_as_an/
- https://www.reddit.com/r/LocalLLaMA/comments/1u37q7u/having_some_fun_with_lmxomni52bhalo_in_open_webui/
