# Lemonade Vision: Late 2026 and Beyond

## Introduction

Agentic coding has upended local AI software development in 2026. We're seeing a rapid increase in the number of important engines upstream of Lemonade, as well as the number of important agents and apps downstream of Lemonade.

In this ecosystem, Lemonade acts as the "app store" for agentic software stacks. A central hub where a curated selection of agents, engines, and models are distributed and "just work" with a single click.


## Vision

The fragmented ecosystem of engines should be abstracted away from users and developers. They should be automatically presented with a "best known configuration" (BKC) for their use case: **auto-BKC**.

In practice:
1. The user selects an agent from a menu of options, such as Hermes, Pi, AnythingLLM, DeepSeek Harness, etc.
2. Lemonade recommends a default model/engine combination that is known to work well with that agent on that system. Users can select something different if they choose.
3. The entire stack of agent, model, and engine is automatically installed and configured. Live in minutes from 1 click.

### Theory

Auto-BKC should be possible because there are two subjective aspects to setting up a local AI agent, and everything else has an objective answer.

The subjective components are:
1. The agent itself, which must meet the user's unique use case.
2. The model, since the results of every agent for every use case vary with model.

> Note: we can recommend a default model for each agent/system combination based on benchmark data.

Once those are selected, everything else should be deterministic:
1. The best engine for the model: objectively selected based on performance data for that system.
2. Installing and configuring the engine to run the model on the target system.
3. Installing and configuring the agent to talk to the engine and the user.

## Enabling Technology

What do we need to build to enable the vision of auto-BKC?

### Performance Database

Deterministically selecting an engine for a target system is possible when we have a comprehensive database of performance for all relevant models on that system.
- Performance: PP, TG, memory, and power. For both the single-user and batched cases.
- Relevant models: the latest releases from Qwen, Meta, Liquid, Google, etc.
- Systems: Strix Halo first. Radeon 9700, Gorgon Halo, and non-AMD systems to follow based on availability.
- Engines: anything that has demonstrated significant community traction; at the time of this writing it should include Halogen, Gufo, and halo-box engines.

#### Correctness

We should adopt a way to assess whether an engine correctly implements a model. Performance data for incorrect implementations should be disregarded. Donato's Terminal Bench Mini is a good candidate.

#### Official Submissions

A baseline of data will be collected using AMD's DevLab cloud.

#### User Submissions

The database should also support user-submitted data, which enables:
- More devices than what is available in DevLab.
- Emergent engines not already on our list.
- Parameter/option permutations not part of our sweeps.

### Engine Integrations

New engines should be integrated into Lemonade as soon as they meet the following criteria:
- Significant community traction.
- Leadership in the performance database.
- Clear differentiation from other engines. Includes non-performance differentiators such as software license, portability, platform support, etc.

Engines will be classified into the following support tiers:
1. Core: recommended for production use with guaranteed support from industry.
2. Community: supported long term by at least one trusted Lemonade maintainer.
3. Experimental: provided by an external party and deployed into a sandbox (e.g., containers, nono.sh). May be removed at any time.

### Agent Manager

Assists users with the installation, configuration, and launch of agents to work with local AI. Contains enough metadata about each agent's use case to enable auto-BKC, along with benchmark data, to recommend engines and models.

Candidate agents: Pi, AnythingLLM, Hermes, DeepSeek Harness, Claude Code, Codex, GitHub CLI.

> Note: we are still working to define this capability in more detail. It will have a phased rollout, first enabling easy-to-deploy developer-first agents (e.g., Pi) before moving on to more complex cases. Model recommendations will be performed by humans until benchmark data is available.

### Smart Router

The smart router enables the agents to easily take advantage of on-prem token servers (e.g., Instinct Coder, Threadripper Halo, etc.) and cloud APIs when available.

### Maintainability

As Lemonade grows, certain measures are needed to ensure that we can sustain both high code velocity and high quality.

#### Container Strategy

Historically Lemonade has preferred to distribute native engines. We're adopting containers as a distribution mechanism for two reasons:
1. Enabling sandboxed deployment, which keeps user's systems and data safer than running native binaries as the `lemonade` user.
2. Minimizing the amount of glue logic needed to integrate new engines and agents.

#### API Contract

The Lemonade Contract is a set of CI tests that assert certain APIs never receive breaking changes. It will solve two maintainability problems:

1. PR reviewers need certainty that a PR they're approving won't have unintended downstream consequences.
2. Downstream apps need certainty that they can take a dependence on these APIs.

#### Planned Refactors

There are a few major Lemonade code modules that recently hit their maintainability limit: PRs became too difficult to correctly review, so we stopped accepting contributions. The refactors will allow for greatly increased code velocity.

1. Model Manager: model_manager.cpp is a ~7000 LoC single file. Needs to be refactored into subclasses with crisp interfaces.
2. API server: sever.cpp is a ~8000 LoC single file. Needs to be refactored onto an endpoint base class, broken into API-specific modules (openai api, lemonade api, etc.), and the API docs should be auto-generated. This will follow the pattern of the recent MCP server refactor.
3. Smart Router: router.cpp is ~3000 LoC file. Needs a clear architecture.

Recently refactored modules that are in a good state include: the CLI, WrappedServer, and MCP server.
