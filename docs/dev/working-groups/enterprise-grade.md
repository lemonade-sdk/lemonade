# Working Group: Enterprise Grade

## Overview

**Lead:** This working group is led by Jeremy Fowers, whose handle is @jeremyfowers on GitHub and @jfowers_amd on Discord.

**Background:** Lemonade has attained traction with users and commercial entities worldwide, and at the same time is seeing a large influx of new PRs and Issues on a daily basis. There is an inherent tension between velocity and robustness that has become top of mind for many stakeholders.

**Goal:** Establish Lemonade as the trusted inference orchestration layer for business-critical deployments.
 - We don't have to choose between velocity and robustness.
 - Side goal: use local AI for as much of the automation as possible, to prove Lemonade and local AI for enterprise use.

## Contributing

Please see the general [contribution guidelines](../contribute.md), then contact @jfowers_amd on Discord before getting started.

See this working group's entry in the [table of working groups](./README.md#table-of-working-groups).

## Roadmap

The overall technical roadmap is to introduce agents, CI enhancements, and dashboards that ensure the quality of Lemonade contributions and releases, while enhancing maintainer bandwidth.

### PR Review Agent

The PR agent will run as part of regular CI and invoke a Lemonade-based Pi agent to assist the maintainers. Core responsibilities of this agent include:
- Review the PR in the context of contribute.md and philosophy.md to point out any misalignment.
- Review against documentation.md to ensure the PR is properly documented.
- Analyze whether the PR introduces major new scope and require a 2nd review from a core maintainer.
- Analyze whether the PR includes any breaking API or UX changes and ensure that they are 1) properly documented and 2) approved by a core maintainer.
- Suggest reviewer(s) for the PR based on the maintainer table in contribute.md.

The PR review agent is intended to guide authors towards a review-ready PR, and to inform reviewers of what to look out for during review. It is not meant to replace human review.

> Note: should eventually consolidate the existing cloud-based PR auto-triage and review automations.

### Issue Review Agent

Analyze all issues (including those that are already open) for:
- Duplicate of another issue.
- Hot issues (lots of likes and/or comments) get a notification onto discord #dev channel.
- Is the issue still relevant, or has it been resolved (e.g., bug fixed in a release) or obsoleted (e.g., gui2 feedback after gui3 release)?
- Assess whether issues are "good": reproducible, clearly defined, and aligned to philosophy.md. If they aren't, auto-reply with guidance and direct the author to the discord.
- Auto-assign "good" issues to maintainers, according to the table in contribute.md, and tag them on discord #dev channel.
- Suggest whether an issue should be labeled as a Good First Issue.

> Note: should eventually consolidate the existing cloud-based issue auto-triage automations.

### Performance Regression Suite

For each engine and backend supported by Lemonade, keep a database of performance data for each tagged release we support. This data is intended for use in:
- When a user wants to maximize performance for a specific model, we can pinpoint the engine/backend/tag combination that would serve them best.
- When upgrading tags, make sure there are not performance regressions on hot models.

The performance regression suite should leverage `lemonade bench` to measure performance on scenarios that are interesting to our audience.

### CI Enhancements

Improve test coverage while reducing unnecessary delays incurred during development.

- Leverage performance database to approve/reject engine/backend upgrades.
- Use Radeon emulation to increase CI coverage of AMD GPUs.
- Add more physical Radeon and Nvidia GPUs to the self-hosted runners pool.
- Increased filtering, so that fewer runners are invoked when narrow changes are introduced.
    - Example: no need to run ROCm inference tests for GUI changes.

### Release Process

Increase the level of automation for releases to improve quality and help us focus on novel work.

- Transition from semantic versioning to date-based versioning so that release version numbers are deterministic.
- Automatically create release branches with the correct version number each week; automatically sync the release tag back to main post-release.
- Allow the github issue for per-release auto-generated release notes to include additional copy that can go at the top of the release page; this will save us from editing the release post-tagging.

### Community Dashboard

Get alerts about Discord activity where attention is required or suggested, such as:
- A new user sent their first message, so we can welcome them.
- Large influx of new users.
- High amount of messages on a single subject.
