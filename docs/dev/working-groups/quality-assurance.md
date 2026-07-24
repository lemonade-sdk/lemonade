# Working Group: Quality Assurance

## Overview

**Lead:** This working group is led by Jeremy Fowers, whose handle is @jeremyfowers on GitHub and @jfowers_amd on Discord.

**Background:** Lemonade is attaining increased traction with users and commercial entities worldwide, and at the same time is seeing a large influx of new PRs and Issues on a daily basis. There is an inherent tension between velocity and robustness that has become top of mind for many stakeholders.

**Goal:** Establish new automated systems including bots, agents, dashboards, and new CI so that we don't have to choose between velocity and robustness.
 - Side goal: use local AI for as many of the automations as possible, to prove Lemonade and local AI for enterprise use.

## Contributing

Please see the general [contribution guidelines](../contribute.md), then contact @jfowers_amd on Discord before getting started.

## Roadmap

### PR Review Agent

The PR agent will run as part of regular CI and invoke a Lemonade-based Pi agent to assist the maintainers. Core responsibilities of this agent include:
- Review the PR in the context of contribute.md and philosophy.md to point out any misalignment.
- Review against documentation.md to ensure the PR is properly documented.
- Analyze whether the PR introduces major new scope and require a 2nd review from a core maintainer.
- Analyze whether the PR includes any breaking API or UX changes and ensure that they are 1) properly documented and 2) approved by a core maintainer.
- Suggest reviewer(s) for the PR based on the maintainer table in contribute.md.
