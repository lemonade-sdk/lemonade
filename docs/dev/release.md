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

## Step 2: Push a Tag

Lemonade releases are automatically created by the [cpp_server_build_test_release.yml workflow](https://github.com/lemonade-sdk/lemonade/blob/main/.github/workflows/cpp_server_build_test_release.yml) final step, which is triggered by pushing a tag that matches the `v*` pattern. The tag must match the pattern `v<major>.<minor>.<patch>`, i.e., the value from CMakeLists.txt with a leading `v`.

Let's say you're releasing v10.7.0, this will trigger the release action:

```
# Make sure you are tagging the right commit!
git checkout main
git pull

git tag v10.7.0

git push origin v10.7.0
```

Example action from v10.7.0: https://github.com/lemonade-sdk/lemonade/actions/runs/27283434473/job/80590025966

## Step 3: Windows Signing

Lemonade .msi artifacts are signed by SignPath.io under their SignPath Foundation program. Thank you SignPath!

Every release must be manually approved on SignPath. After the Windows installer is built by the release action, a job called Sign MSI Installers with SignPath will start.

Example from v10.7.0: https://github.com/lemonade-sdk/lemonade/actions/runs/27283434473/job/80587971984

The log will include a link like this:

```
You can view the signing request here: https://app.signpath.io/Web/8103545b-7814-4edc-86d6-a91dc2a2291b/SigningRequests/7eab3b1c-0ad2-4a65-93a5-683a8065e926
```

You must click the link, sign in, and press `Approve`.

## Step 4: Release Action Finishes

Wait for the `cpp_server_build_test_release.yml` action to complete successfully. Sometimes it doesn't, because of a false negative on a test or because we renamed a workflow step or release artifact and forgot to update the release step.

If it fails, ideally you can press "rerun" on the job and get success.

If you need to fix the code, first push the fix to `main` branch. Then tag the head of main with the release tag and force push it to origin.

## Step 5: Update the Release Notes

The release action auto-generates a set of release notes that looks like this: https://github.com/lemonade-sdk/lemonade/releases/tag/v10.7.0

Notably, two sections are not auto-generated: the headline and breaking changes.

You may also need to edit the release to:
- add co-author contributors who were missing
- add deprecation notices
- add/remove links to release artifacts that we forgot to update in the release workflow.

DO NOT add or replace release artifacts, as this would break the chain of custody for the release and damage our credibility with users.

### Headline

The headline section must start with `## Headline` and contain a single-depth bulleted list, because https://lemonade-server.ai looks for this pattern.

The list should be the 3-5 most noteworthy aspects of the release that you would want users to know about. Keep these concise and do not include meta info like shoutouts. Headlines must also not include any special formatting or links.

### Breaking Changes

Bulleted list with one item per breaking change. Keep it concise, and link to a wiki article for migration if more details are needed.

## Step 6: Discord Announcement

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

## Step 7: Social Media

Not required, but it is always good to promote the new release online.
