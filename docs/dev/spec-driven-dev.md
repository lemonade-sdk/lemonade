# Spec Driven Development

The Lemonade maintainers decide which PRs to review based on a specification-driven development policy. This policy applies to any change that would alter the intended scope, surface area, or user experience of Lemonade.

We implement spec-driven development by requiring any such change to open a request for comment (RFC) discussion [on the Lemonade repository](https://github.com/lemonade-sdk/lemonade/discussions). These RFCs stay open until maintainers have had an opportunity to review, comment, and approve. PRs that implement qualifying changes must link an approved RFC. Please note that the [AI content policy](./ai-content-policy.md) strictly applies in these discussions.

> [!TIP]
> Changes that do none of these, for example a bug fix that aligns the realized user experience to the intended experience, are exempt from this policy.

> [!TIP]
> When in doubt, assume this policy applies. Some changes may be minor enough to not require this policy, but maintainers can request that any PR close, follow this policy, and then re-open.

## Definitions

This section helps you identify which changes should follow the policy.

* **Scope: what does Lemonade do, and not do?** For example, `lemond` serves LLM inference over endpoints to clients. It does not provide an agentic memory system as of this writing; that would be a scope increase.
* **Surface area: many capabilities does Lemonade have, within its scope?** For example, `lemonade` CLI supports saving your model loading options to a configuration file. As of this writing, it does not support commands for viewing or editing these options; that would be a surface area increase.
* **User experience: how do end-users and devs interact with Lemonade?** This is a broad category that covers anything that would change the experience from one release to the next. It includes breaking changes, visual design language, GUI layout, installation/distribution, testing, etc.

## RFC Sizes

The amount of detail provided in an RFC should be proportional to the size of the change being proposed.

Here are 3 general tiers:

1. **Working group proposal:** [working groups](./working-groups/README.md) define large scope increases that many people collaborate on, over the course of multiple release cycles. RFCs that propose a new working group should be highly detailed.
2. **Major feature proposal:** features large enough to span multiple PRs should go into significant detail.
3. **Minor feature proposal:** si

## Development Lifecycle

This section helps you understand how to take your idea to production as part of Lemonade.

The steps are:

1. Open an RFC discussion [on the Lemonade discussions tab](https://github.com/lemonade-sdk/lemonade/discussions). Make sure to select the `Request for Comment (RFC)` category. See the [Writing your RFC](#writing-your-rfc) section below for guidance on how to ensure your RFC is well received.
2. Tag the relevant maintainers on the RFC. You can check the [maintainers table](./contribute.md#maintainers) to see each maintainer's area of interest. You should also bring your RFC to the #dev channel on the discord to help raise awareness amongst other contributors. RFCs with more votes may be considered before RFCs with fewer votes.
3. RFCs start with the `rfc:open-for-comment` label, which indicates no decision has been made yet about whether or when a PR should be opened.
4. Make sure to regularly update the body of your RFC when the idea progresses or changes. Provide a list of updates at the top (e.g., `Update 3: Changed X to Y`) to help people follow along.
5. When the maintainers are happy with the RFC body, @jeremyfowers will provide a final review and change the label to `rfc:on-roadmap`.
6. Open your PR and link the RFC in the appropriate field of the PR body template.
7. The PR should faithfully implement the RFC. If the PR has significant scope, surface area, or user experience divergence from the RFC, request @jeremyfowers to review the PR.

## Writing your RFC

This section helps you understand how to write a polished RFC that is likely to get a good reception with the community. The `Request for Comment (RFC)` discussion category also has a template that will guide you.

Reminder: the [AI content policy](./ai-content-policy.md) strictly applies to RFCs. Important decisions in Lemonade must be made through human-to-human discourse.

<!-- if you ever edit this section, make sure to update the template too! -->

Every RFC should have the following sections:

1. User story: people need to accomplish task X with Lemonade. The benefit of enabling X in Lemonade is Y, compared to other available solutions.
2. Breaking changes (if any): The negative impact of the associated breaking changes is Z.
3. High-level design: what are you changing in Lemonade, from the perspective of a user or developer? Do not cite code here.
4. Design details (if necessary): what parts of the codebase are you modifying, and how?
5. Test plan: how will human testers help ensure the change is fully robust? How will CI testing protect the change?
6. Maintenance plan: is the feature fully maintained by CI, or does it need to be continuously maintained through human intervention (e.g., llama.cpp adds support for new models in their releases; human maintainers must merge submit PRs to provide support in Lemonade).
7. Security considerations (if any)
