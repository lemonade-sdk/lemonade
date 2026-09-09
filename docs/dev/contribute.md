# Contributing to Lemonade

We're excited that you are interested in contributing!

Please carefully review Lemonade's [philosophy](./philosophy.md) before making a pull request. As a contributor, you can anticipate the vast majority of reviewer feedback by aligning your design to the philosophy.

## Development Process

### Working Groups

Lemonade's roadmap is defined by a set of [working groups](./working-groups/README.md), and most substantial contributions should be within the scope of one of these groups. If you would like to participate, start by reading this document and then reach out to the working group lead in your subject of interest.

### Merging a Contribution

1. We highly recommend that all contributors join the [Lemonade discord community](https://discord.gg/5xXzkMu8Zk), meet [the maintainers](#maintainers) and get a sense of what is trending.
2. Depending on the complexity of your contribution:
    - Simple fixes: just make a PR.
    - Features: contact [a maintainer](#maintainers) who works in the area of your feature and get them to agree to review it before you start coding.
    - Architectural and major scope changes: write an issue explaining the justification and design and bring it to the Discord for debate.
3. Create a fork of Lemonade repo, implement your code, and then make a pull request to merge your code back to the main repo. Assign the reviewer(s) you discussed the change with.

### Picking a Project

Not sure what to work on? Come to the feature-requests and troubleshooting channels on the Discord and see what people need!

### Adding a Backend

Inference backends are self-describing: a backend is a descriptor (plain data) plus a server class, and everything else (router, CLI, `/system-info`, docs) is derived from it. See [Adding a backend](./adding-a-backend.md) for the full contract and a minimal example. Please post in the dev channel on Discord before starting work.

### Issues

Issues are a great way to document a bug or feature request. However, Lemonade is a community-driven project and you still need to find someone to implement your issue. It is highly recommended that you bring your issue to the [Lemonade discord community](https://discord.gg/5xXzkMu8Zk) and connect with a contributor who wants to implement it.

### Reviewer Expectation

Each contribution needs to:

1. Accurately describe the scope, use case, and implementation in the PR description.
2. Solve one clearly defined problem, and limit its scope to what is necessary.
3. Adhere to the [project philosophy](./philosophy.md).
4. Pass the CI tests and follow the [testing guide](./testing.md).
    - Contributors: make sure the code builds locally before creating the PR.
    - Reviewers: make sure to check the code *before* allowing CI to run!
5. Meet the requirements of the [documentation guide](./documentation.md).
6. Sustain the overall code quality and standards of the rest of the repo.

The fastest way to build trust as a new contributor is to submit small, clear, well-tested PRs that are easy to review and easy to verify.

### AI Contribution Policy

Contributors are welcome to use AI tools while working on Lemonade. However, contributors remain fully responsible for the code they submit.

If you use AI-assisted coding, please make sure that you:

1. Understand the generated code before submitting it.
2. Review the full diff yourself.
3. Remove unrelated or unnecessary changes.
4. Test the result locally where possible.

AI can make it easy to generate large changes, but large or unfocused changes are harder for maintainers to review and are less likely to be accepted quickly.

__Please do not use AI to write issues__. If you feel an issue is important enough for a human to read it then please take the time to write it yourself.

## Review Process

1. Work that is ready for CI testing and AI review, but not ready for human review, should be marked as a draft PR.
2. Please use an AI review tool such as Claude Code's `/review` on your own code to flag and fix problems before marking the PR ready for human review.
3. A "ready for review" non-draft PR is a signal that it is ready for human review.
4. Evaluate the PR for scope:
    - minor features and fixes should have any 1 reviewer.
    - major features, breaking changes, refactors, new backends, security-related issues, etc. should have 2 reviewers including 1 subject area expert.
    - project scope expansion, re-architecture, design language changes, etc. should have @jeremyfowers review.
5. Evaluate the PR for which subject areas it impacts, and request review from a subject area expert in the maintainers table below.

## Maintainers

While each maintainer is welcome to work on any part of the Lemonade codebase, each maintainer does have specific knowledge of certain areas. You should use their knowledge as a starting point for designing your contribution, and they will be the ones to review your contribution when it is ready.

"Admin maintainer" means that individual is a repository admin who can tag releases and take other administrative actions.

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
