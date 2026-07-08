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
    - Architectural changes: write an issue explaining the justification and design and bring it to the Discord for debate.
3. Create a fork of Lemonade repo, implement your code, and then make a pull request to merge your code back to the main repo. Assign the reviewer(s) you discussed the change with.

### Picking a Project

Not sure what to work on? Come to the feature-requests and troubleshooting channels on the Discord and see what people need!

### Adding a Backend

Inference backends are self-describing: a backend is a descriptor (plain data) plus a server class, and everything else (router, CLI, `/system-info`, docs) is derived from it. See [Adding a backend](./adding-a-backend.md) for the full contract and a minimal example.

### Issues

Issues are a great way to document a bug or feature request. However, Lemonade is a community-driven project and you still need to find someone to implement your issue. It is highly recommended that you bring your issue to the [Lemonade discord community](https://discord.gg/5xXzkMu8Zk) and connect with a contributor who wants to implement it.

### Reviewer Expectation

Each contribution needs to:

1. Adhere to the [philosophy](./philosophy.md).
2. Pass the CI tests.
    - Contributors: make sure the code builds locally before creating the PR.
    - Reviewers: make sure to check the code *before* allowing CI to run!
3. Sustain the overall code quality and standards of the rest of the repo.

### Making Pull Requests Easy to Review

Maintainers try to review contributions quickly, but review time is limited. The easiest pull requests to review are small, focused, and easy to verify.

Before opening a PR, please make sure that:

1. The PR solves one clear problem.
2. The changes are limited to what is necessary.
3. There are no unrelated local changes included.
4. The PR description explains what changed and why.
5. The PR describes how the change was tested, including any platforms that could not be tested locally.
6. Larger refactoring is separated from bug fixes or feature changes.

Some areas of the codebase are more sensitive than they may appear. Small changes can sometimes have large side effects, especially in recently reworked, complex, or platform-specific code. In these cases, reviewers may ask you to reduce the scope, add tests, or follow a specific implementation approach. If you need help testing on a platform you do not have access to, please mention it in the PR and ask for support in the Lemonade Discord.

The fastest way to build trust as a new contributor is to submit small, clear, well-tested PRs that are easy to review and easy to verify.

### AI Policy

Contributors are welcome to use AI tools while working on Lemonade. However, contributors remain fully responsible for the code they submit.

If you use AI-assisted coding, please make sure that you:

1. Understand the generated code before submitting it.
2. Review the full diff yourself.
3. Remove unrelated or unnecessary changes.
4. Test the result locally where possible.
5. Keep the PR especially small and focused.

AI can make it easy to generate large changes, but large or unfocused changes are harder for maintainers to review and are less likely to be accepted quickly.

Reviewers are expected to use tools like Claude Code's `/review` to save time by analyzing code quality and edge cases. If AI tools generate PR comments, please clearly identify which comments are AI-generated and which are authored by you.

__Please do not use AI to write issues__. If you feel an issue is important enough for a human to read it then please take the time to write it yourself.

AI tools may be used to draft documentation, but the author is responsible for verifying accuracy before submitting. See the [documentation guide](./documentation.md) for a pre-submission checklist and style guidance.

## Maintainers

While each maintainer is welcome to work on any part of the Lemonade codebase, each maintainer does have specific knowledge of certain areas. You should use their knowledge as a starting point for designing your contribution, and they will be the ones to review your contribution when it is ready.

### Features

| Subject area | Maintainers |
| --- | --- |
| GUI | Primary maintainer needed, @danielholanda, @sofiageo, @jeremyfowers |
| CLI | @bitgamma (discord: mikkoph), @jeremyfowers |
| Networking (HTTP) | @Geramy, @bitgamma (discord: mikkoph) |
| Mobile Apps | @Geramy |
| Coding Agents | @sawansri |
| Website | @jeremyfowers |
| Content and guides | @vgodsoe |
| LemonAIde bot | @kpoineal (discord: primaL-) |

### AI Support

| Subject area | Maintainers |
| --- | --- |
| LLMs | @sawansri, @bitgamma (discord: mikkoph), @ramkrishna2910 |
| Speech AI | @bitgamma (discord: mikkoph), @ramkrishna2910 |
| Image AI | @bitgamma (discord: mikkoph), @kenvandine, @ramkrishna2910 |

### OS Support

| Subject area | Maintainers |
| --- | --- |
| Linux | @superm1, @kenvandine, @bitgamma (discord: mikkoph), @sofiageo |
| Ubuntu, Debian, and Snap | @kenvandine, @superm1 |
| Arch | @sofiageo, @bitgamma (discord: mikkoph) |
| Fedora | @valiabhay |
| macOS | @Geramy, @ramkrishna2910 |
| Windows | @kpoineal (discord: primaL-), @jeremyfowers |
| Docker | @siavashhub (discord: icmpreply) |

### Backend Support

| Subject area | Maintainers |
| --- | --- |
| Adding new backends | @ramkrishna2910, @bitgamma (discord: mikkoph), @jeremyfowers |
| AMD ROCm | @superm1, @Geramy, @danielholanda |
| AMD NPU | @ramkrishna2910, @jeremyfowers |
| Intel | Primary Maintainer Needed, @Geramy |
| Nvidia | Maintainer Needed |
| vLLM | @ramkrishna2910 |
| MLX | @Geramy |
