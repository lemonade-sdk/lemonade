# Spec Driven Development Policy

Lemonade's maintainers strive to ensure that Lemonade has a great user experience, a coherent scope, and a high degree of robustness. One of our main tools to accomplish this is specification-driven development: the idea that changes to Lemonade's scope, surface area, and/or user experience should be discussed upfront before a PR is opened.

These conversations take place on request for comment (RFC) discussions [on the Lemonade repository](https://github.com/lemonade-sdk/lemonade/discussions). These RFCs stay open until maintainers have had an opportunity to review, comment, and approve. Please note that the [AI content policy](./ai-content-policy.md) strictly applies in these discussions.

> [!TIP]
> Bug fixes that align the realized user experience to the intended experience are exempt from this policy.

> [!TIP]
> When in doubt, assume this policy applies. Some changes may be minor enough to not require this policy, but maintainers can request that any PR close, follow this policy, and then re-open.

### Definitions

This section helps you identify which changes should follow the policy.

* **Scope: what are Lemonade's capabilities?** For example, `lemond` serves LLM inference over endpoints to clients. It does not provide an agentic memory system as of this writing; that would be a scope increase.
* **Surface area: how are Lemonade's capabilities exposed?** For example, Lemonade is capable of saving model loading options to a configuration file. As of this writing, `lemonade` CLI supports saving options, but it does not support viewing or editing these options; adding these would be a surface area increase.
* **User experience: how do users interact with Lemonade's surface area?** This is a broad category that covers anything that would change the experience from one release to the next. It includes breaking changes, visual design language, GUI layout, installation/distribution, etc.

Note that the above applies to developers of Lemonade, as well as end-users. Anything that changes the scope, surface area, or experience of developing Lemonade also should have an RFC. This includes, but is not limited to: changing the release process, adding new tests, refactoring working code, etc.

### Types of RFCs

Here are 3 general tiers:

1. **Working group proposal:** [working groups](./working-groups/README.md) define large scope increases that many people collaborate on, over the course of multiple release cycles. RFCs that propose a new working group should be reviewed by many project maintainers.
2. **Major feature proposal:** features large enough to span multiple PRs should go into significant detail and tag the all the project maintainers in that area.
3. **Minor feature proposal:** simpler features (e.g., adding a flag to a CLI subcommand) do not need as much detail and only need to tag 1-2 maintainers.

> [!TIP]
> If you are planning to make a lot of contributions to Lemonade, we encourage you to join or found a working group. A ratified working group can make any amount of PRs within the scope of its charter without needing to open additional RFCs.

## Development Lifecycle

This section helps you understand how to take your idea to production as part of Lemonade.

The steps are:

1. Open an RFC discussion [on the Lemonade discussions tab](https://github.com/lemonade-sdk/lemonade/discussions). Make sure to select the `Request for Comment (RFC)` category. See the [Writing your RFC](#writing-your-rfc) section below for guidance on how to ensure your RFC is well received.
2. Tag the relevant maintainers on the RFC. You can check the [maintainers table](./contribute.md#maintainers) to see each maintainer's area of interest. You should also bring your RFC to the #dev channel on the discord to help raise awareness amongst other contributors. RFCs with more votes may be considered before RFCs with fewer votes.
3. RFCs start with the `rfc:open-for-comment` label, which indicates no decision has been made yet about whether or when a PR should be opened.
4. Make sure to regularly update the body of your RFC when the idea progresses or changes. Provide a list of updates at the top (e.g., `Update 3: Changed X to Y`) to help people follow along.
5. Once the RFC body converges the code owners will provide a final review and change the label to `rfc:on-roadmap`. If the maintainers decide the RFC should not be implemented it will be assigned the `rfc:not-planned` label.
6. Open your PR and link the RFC in the appropriate field of the PR body template.
7. The PR should faithfully implement the RFC. If the PR has significant scope, surface area, or user experience divergence from the RFC, request the relevant code owner to review the PR.

### Code Owners

The following maintainers are required to review RFCs within their subject area. Other RFCs can be reviewed by other maintainers, as long as it has been sufficiently reviewed.

| Code Owner    | Area                                          |
|---------------|-----------------------------------------------|
| @bitgamma     | `lemonade` CLI                                |
| @kpoineal     | `Lemonade App` GUI                            |
| @jeremyfowers | new endpoints, new backends, breaking changes |


## Writing your RFC

This section helps you understand how to write a polished RFC that is likely to get a good reception with the community. The `Request for Comment (RFC)` discussion category also has a template that will guide you.

Reminder: the [AI content policy](./ai-content-policy.md) strictly applies to RFCs. Important decisions in Lemonade must be made through human-to-human discourse.

<!-- if you ever edit this section, make sure to update the template too! -->

Every RFC should have the following sections:

1. User story: people need to accomplish task X with Lemonade. The benefit of enabling X in Lemonade is Y, compared to other available solutions.
2. High-level design: what are you changing in Lemonade, from the perspective of a user or developer? Do not cite code here.
3. Breaking changes (if any): The negative impact of the associated breaking changes is Z.
4. Maintenance plan:
   - Is the feature fully maintained by CI, or does it need to be continuously maintained through human intervention (e.g., llama.cpp adds support for new models in their releases; human maintainers must merge submit PRs to provide support in Lemonade).
   - Are human testers needed on the PR?
   - How will CI testing protect the change?
5. Risks (e.g., security considerations; if any)
