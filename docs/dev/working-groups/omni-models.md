# Working Group: Omni Models

## Status

This working group is **archived** because the maintainers feel we have accomplished the core objective. Omni modality is still important to this project and will continue maintaining and extending it.

## Overview

**Lead:** This working group is led by Jeremy Fowers, whose handle is @jeremyfowers on GitHub and @jfowers_amd on Discord.

**Background:** Lemonade Omni Models combine multiple models and backends together to present omni-modal capabilities (e.g., chat and image editing in the same model) that would otherwise not be readily available in local AI.

**Why:** Omni-modality enables natural interactions between end-users and local AI systems, and we believe it is essential for mass adoption of local AI.

**Goal:** Improve Omni Models to become a staple of local AI apps and use. The condition for this is not yet apparent, so we will start by iterating on some obvious next steps.

## Roadmap

> Roadmap items may be high-level objectives that may span multiple issues and PRs. Specific issues and PRs can be found in the [Omni Models GitHub Milestone](https://github.com/lemonade-sdk/lemonade/milestone/11).

- [x] Develop application-specific LMX skills, system prompts, and tools to be more useful for workflows such as image generation.
    - NOTE: we did one of these with Halo Tales, but it would be cool to have more.
- [x] Update the Lemonade website and top-level README to emphasize the developer journey, towards embedding lemond and integrating LMX models.
- [x] Lemonade Mix (LMX) omni models are available and useful in Open WebUI. Publish a blog explaining LMX and showing use cases.
- [x] Publish Halo Tales, a reference application based on LMX models. Update Lemonade to smooth over any rough edges. Then, publish a blog explaining the LMX developer journey and learnings from making Halo Tales.
    - NOTE: HaloTales wound up being a specialized LMX instead of an app.
- [x] Migrate LMX model definitions to Hugging Face to make them searchable as models.
    - [ ] Make sure Lemonade is shown under Use This Model on the model card.
