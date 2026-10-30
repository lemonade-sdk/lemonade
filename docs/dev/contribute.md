# Contributing to Lemonade

We're excited that you are interested in contributing!

Before contributing, please familiarize yourself with these 3 documents:
1. The [project philosophy](./philosophy.md) will help you understand design priorities.
2. The [AI content policy](./ai-content-policy.md) is essential to make sure your communications are well received.
3. Understanding the [spec-driven development guide](./spec-driven-dev.md) is required before opening a pull request.

We also highly recommend that all contributors join the [Lemonade discord community](https://discord.gg/5xXzkMu8Zk), meet [the maintainers](#maintainers) and get a sense of what is trending.

Welcome to the project!

## Development Process

### Working Groups

Lemonade's roadmap is defined by a set of [working groups](./working-groups/README.md). Most substantial contributions should be within the scope of an existing group, and you can join by reaching out to the leads.

If you want to found a new working group, or make any change to Lemonade's scope, surface area, or user experience, you need to follow the [spec-driven development process](./spec-driven-dev.md).

### PR Expectation

Each contribution needs to:

1. Accurately describe the scope, use case, and implementation in the PR body. This must be human-written per the [AI content policy](./ai-content-policy.md).
2. Solve one clearly defined problem, and limit its scope to what is necessary.
3. Pass the CI tests and follow the [testing guide](./testing.md).
    - Contributors: make sure the code builds locally before creating the PR.
    - Reviewers: make sure to check the code *before* allowing CI to run!
4. Meet the requirements of the [documentation guide](./documentation.md).
5. Sustain the overall code quality and standards of the rest of the repo.

The fastest way to build trust as a new contributor is to submit small, clear, well-tested PRs that are easy to review and easy to verify.

### Opening a PR

1. Depending on the complexity and nature of your contribution:
    - Simple fixes: just make a PR.
    - Already within the scope of a [working group](./working-groups/README.md): just make a PR, and link the working group in the PR body.
    - Anything else: [open an RFC](./spec-driven-dev.md), get it approved, and link it in the PR body.
2. Create a fork of Lemonade repo and implement your code.
3. You are strongly encouraged to run an AI code review tool yourself, such as `claude /code-review` and address any issues before opening your PR. Reviewers may put your PR back to draft status and ask you to do this.
4. Make a pull request to merge your code back to the main repo.
5. A Lemonade administrator will triage your PR and assign it a label:
  - `rfc:not-required` or `rfc:on-roadmap`: ready for review.
  - `rfc:required`: PR will be marked as a draft until an approved RFC or working group is linked.

## Maintainers

While each maintainer is welcome to work on any part of the Lemonade codebase, each maintainer does have specific knowledge of certain areas. You should use their knowledge as a starting point for designing your contribution, and they will be the ones to review your contribution when it is ready.

"Admin maintainer" means that individual is a repository admin who can tag releases and take other administrative actions.

> [!TIP]
> Whenever possible, PRs should have at least one reviewer with subject area expertise. Try to avoid merging PRs that are approved, but not thoroughly reviewed.

| Maintainer        | Admin | Subject Areas                                                                                                                                    |
|-------------------|-------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| @jeremyfowers     | Yes   | new endpoints, new backends, large new features, GUI design language, new CLI commands, website, governance, Lemonade Mix (LMX) omni models, ci  |
| @kenvandine       | Yes   | snaps, linux, new backends, hardware vendor support, Nvidia CUDA, ARM                                                                            |
| @ramkrishna2910   | Yes   | new backends, new modalities, vLLM, whisper, stable diffusion, smart router and orchestration, cloud API integration, external partnerships, NPU |
| @superm1          | Yes   | ROCm, linux packaging, system info, security, Docker, containers, llamacpp, NPU , FastFlowLM                                                     |
| @abn              |       | telemetry, sandboxing, http                                                                                                                      |
| @bitgamma         |       | thenoise, cli, recipes, new backends, benchmarking, new models                                                                                   |
| @eddierichter-amd |       | smart router                                                                                                                                     |
| @fl0rianr         |       | Lemonade Mix (LMX) omni models, GUI, ci, smart router                                                                                            |
| @Geramy           |       | sockets, tcp/ip, udp, named pipes, mac, security, Nexus mesh compute                                                                             |
| @kpoineal         |       | GUI, app, a11y, Windows, LemonAIde                                                                                                               |
| @pwilkin          |       | Trellis, OpenMOSS, ACE-Step, ThinkSound, llamacpp                                                                                                |
| @sawansri         |       | agents, tui, cli, new backends, launch, vLLM, new models                                                                                         |
| @siavashhub       |       | MCP, chat repl, Docker, containers                                                                                                               |
| @SlawomirNowaczyk |       | smart router                                                                                                                                     |
| @sofiageo         |       | linux packaging, flatpak, GUI design language, benchmarking                                                                                      |
| @valiabhay        |       | Fedora                                                                                                                                           |
