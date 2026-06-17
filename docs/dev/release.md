# Lemonade Release Process

This guide documents the end-to-end process of releasing Lemonade.

## Quality

Lemonade has built its brand on quality and ease-of-use. Do not release a new Lemonade version if this is compromised in any way. Carefully monitor the state of the upcoming release using [https://lemonade-server.ai/repo-manager](https://lemonade-server.ai/repo-manager).

## Release Cadence

Lemonade operates on a weekly release cadence, with new releases coming out each Wednesday morning. Each release is managed by a core maintainer on a rotating basis.

It is highly recommended that any PR targeting a particular release be in a highly-reviewed state by Monday morning on the week of that release. Maintainers reserve the right to postpone merging any PR until after the release, in order to prioritize quality.

## Step 1: Update the Version

The single source of truth for Lemonade's version number is [near the top of CMakeLists.txt](https://github.com/lemonade-sdk/lemonade/blob/20126a43e38aae5c71a8a34d205cd913917e4466/CMakeLists.txt#L8). This number must be updated prior to creating a release.

Version numbers should always have the format `major.minor.patch`.

Decide the new version number using these criteria:
- `major` release: Must include either a feature that fundamentally changes how people see Lemonade or highly disruptive breaking changes (we try to avoid those now...)
    - Discuss with other core maintainers before changing the major version.
    - Examples of past major releases: changing from Python to C++, adding NPU support on Linux.
- `minor` release: Includes at least one headlining feature with broad appeal.
    - Most releases are `minor` releases because code velocity on this project is high enough that a new headline feature lands every couple of days.
- `patch` release: release only contains fixes or less-visible features.
    - `patch` releases are rare, and typically only happen if a serious bug needs to be fixed right after a `major`/`minor` release.

## Step 2: Create the Release Branch

Each release is developed and tagged from a dedicated release branch, not from `main`. Create the branch from the tip of `main` once the release is ready to stabilize:

```bash
git checkout main
git pull
git checkout -b release-vX.Y.Z
git push origin release-vX.Y.Z
```

### Managing the Release Branch

Two situations arise while the release branch is open:

**Backport a fix from `main` to the release branch** (most common — a fix merged to main that should also land in this release):

```bash
# Find the squash-merge commit SHA on main
git log main --oneline | head -10

git checkout release-vX.Y.Z
git cherry-pick <commit-sha>
git push origin release-vX.Y.Z
```

The release branch has no squash-merge PR requirement, so a direct push works fine.

**Forward-port a commit from the release branch back to `main`** (rare — a fix made directly on the release branch that was never on main):

Because `main` requires squash merges for PRs, **do not use a PR** — it would collapse the cherry-picked commit into a squash commit, destroying authorship and history. Push directly with an admin bypass instead:

```bash
git checkout main
git pull
git cherry-pick <commit-sha-from-release-branch>
git push origin main   # requires admin "bypass branch protections" permission
```

Confirm with the team before pushing directly to `main`.

## Step 3: Push a Tag

Lemonade releases are automatically created by the [cpp_server_build_test_release.yml workflow](https://github.com/lemonade-sdk/lemonade/blob/main/.github/workflows/cpp_server_build_test_release.yml) final step, which is triggered by pushing a tag that matches the `v*` pattern. The tag must match the pattern `v<major>.<minor>.<patch>`, i.e., the value from CMakeLists.txt with a leading `v`.

Let's say you're releasing v10.8.0, this will trigger the release action:

```bash
# Make sure you are tagging the right commit!
git checkout release-v10.8.0
git pull

git tag v10.8.0

git push origin v10.8.0
```

Example action from v10.7.0: https://github.com/lemonade-sdk/lemonade/actions/runs/27283434473/job/80590025966

## Step 4: Windows Signing

Lemonade .msi artifacts are signed by SignPath.io under their SignPath Foundation program. Thank you SignPath!

Every release must be manually approved on SignPath. After the Windows installer is built by the release action, a job called Sign MSI Installers with SignPath will start.

Example from v10.7.0: https://github.com/lemonade-sdk/lemonade/actions/runs/27283434473/job/80587971984

The log will include a link like this:

```
You can view the signing request here: https://app.signpath.io/Web/8103545b-7814-4edc-86d6-a91dc2a2291b/SigningRequests/7eab3b1c-0ad2-4a65-93a5-683a8065e926
```

You must click the link, sign in, and press `Approve`.

## Step 5: Release Action Finishes

Wait for the `cpp_server_build_test_release.yml` action to complete successfully. Sometimes it doesn't, because of a false negative on a test or because we renamed a workflow step or release artifact and forgot to update the release step.

If it fails, ideally you can press "rerun" on the job and get success.

If you need to fix the code, push the fix to the release branch. Then move the tag to the new head and force push it to origin:

```bash
git checkout release-vX.Y.Z
# ... make fix, commit ...
git push origin release-vX.Y.Z
git tag -f vX.Y.Z
git push origin vX.Y.Z --force
```

## Step 6: Update the Release Notes

The release action auto-generates release notes that include the **Headline** and **Breaking Changes** sections pulled from the open GitHub issue titled `vX.Y.Z release notes` (exact match on the tag name, e.g. `v10.8.0 release notes`). The result looks like this: https://github.com/lemonade-sdk/lemonade/releases/tag/v10.7.0

You may need to manually edit the release to:
- add co-author contributors who were missing
- add deprecation notices
- add/remove links to release artifacts that we forgot to update in the release workflow.

DO NOT add or replace release artifacts, as this would break the chain of custody for the release and damage our credibility with users.

### Headline

The headline section starts with `## Headline` and contains a single-depth bulleted list. The website at https://lemonade-server.ai parses this section, so the format must be preserved.

The list should be the 3-5 most noteworthy aspects of the release. Keep items concise; no special formatting or links, no shoutouts.

### Breaking Changes

Bulleted list with one item per breaking change. Keep it concise, and link to a wiki article for migration if more details are needed.

## Step 7: Discord Announcement

Each release gets a post in `#announcements` on the Lemonade Discord.

- Major and Minor releases: `@everyone`
- Patch releases: `@release`

This announcement should create excitement for the release and give shout outs to everyone who contributed

Suggested structure:
- Introduction sentence
- News (if any)
- Breaking changes: copied from release notes
- One section per headline from the release notes, with 1-2 sentences explaining it.
- Additional Improvements section with a bulleted list, one bullet per notable contribution

Try to find an appealing narrative arc for the release, and implement it by combining contributions into sections/bullets. For example, if there were 3 contributions by 3 authors for LMX models, say "Authors X, Y, and Z teamed up to improve in LMX..."

## Step 8: Social Media

Not required, but it is always good to promote the new release online.
