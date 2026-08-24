# Developing Lemonade

Lemonade is a community-driven project organized around the [Lemonade Discord server](https://discord.gg/5xXzkMu8Zk). That should be your first stop to meet the developers, get support, and propose contributions.

This documentation is here to help you set up your environment, start modifying Lemonade to your liking, and join the developer community!

### Developer Setup

Start here: [getting started](./getting-started.md).

You can also reference the [app](./app.md) and [web-ui](./web-ui.md) guides to learn more about the GUI side of the project.

### Philosophy

Understand Lemonade's mission and design tenets before contributing by reading the [philosophy](./philosophy.md).

### Roadmap

Lemonade's roadmap is defined by a set of [working groups](./working-groups/README.md), and most substantial contributions should be within the scope of one of these groups.

### Contributing

The Lemonade project welcomes contributions! Learn about the project's mission, maintainers, and contribution process [here](./contribute.md).

### Documentation

Writing or improving docs? Read the [documentation guide](./documentation.md) for style, structure, and guidance on AI-assisted contributions.

### Testing

Preparing a PR? Read the [testing guide](./testing.md) to learn which test suites cover your change and what reviewers expect.

### Lemonade Omni Models

Lemonade has a unique capability to group LLM, image, and speech models together to present a unified omni-modal "model" to end-users. These one-click bundles are called Lemonade Omni Models, and they're routed via an internal mechanism called OmniRouter. Learn more [here](./lemonade-omni.md).

### CI System

Lemonade has a CI system that tests changes on real AI PC hardware targets — in the merge queue by default, or on a labeled PR (the [testing guide](./testing.md#what-defers-to-the-merge-queue) maps each `ci:*` label to the jobs it opts in). The [self-hosted runners](./self-hosted-runners.md) guide documents how those are set up.

### Performance Diagnostics

The [large-request memory benchmark](./large-request-memory-benchmark.md)
documents the manual Linux reproducer for allocator high-water retention and
its real-backend validation mode.
