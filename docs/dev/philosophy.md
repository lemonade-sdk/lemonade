# Lemonade Philosophy

## Mission

Our goal is to meaningfully increase the amount of genuinely useful local AI use cases and applications in the ecosystem.

## Fundamentals

Lemonade is designed to make high-performance local AI more accessible to users and builders. We do this by:

1. Making inference engines and models easy to deploy cross-platform.
2. Building abstractions to unlock new capabilities.
3. Providing visibility to builders and apps.

## Design Tenets

These are the practices that keep Lemonade's code aligned to its mission.

### Lemonade is the Foundation

We want you to get started in our app, then connect Lemonade to other apps, and then embed Lemonade in your own app. Lemonade becomes progressively more invisible throughout this process, and we're cool with that. Lemonade is the foundation, not the house.

Lemonade has a GUI for two reasons only:

1. To show you what's possible. We know that if a feature doesn't have a GUI to show it off, then it doesn't exist for most people.
2. To help you manage models and backends across connected apps.

### Prioritize the Happy Path

Lemonade needs advanced features to facilitate innovation, but advanced features should not add friction to the new user journey.

<details>
<summary>Examples</summary>

* *I want to make multiple AMD ROCm™ software distributions available for end-users, enthusiasts, and software vendors.*
    * Challenge: new users should have a default they can happily never worry about.
    * Solution: ROCm distribution channel is a well-documented configuration setting; the default channel is the best channel for new users.

* *I want to add a backend manager to the GUI to help organize Lemonade's ever-growing (10+) backends.*
    * Challenge: The Lemonade v9 GUI layout was not scalable, so any new panel/button would add major clutter.
    * Solution: add a VS Code-style sidebar to the left panel, allowing it to switch between Model and Backend management. Most users are familiar with VS Code, making this interface simultaneously minimal and discoverable.

</details>

### Standards are Intuitive

Nobody is born knowing how to use a computer but lots of people are familiar with the conventions of popular software. In the AI space, examples include the OpenAI API, VS Code GUI, and Ollama CLI.

Any time we have an opportunity to adhere to a standard, we should, because it saves the friction of teaching our user base a new concept. Any time we're introducing a new feature, we should figure out if it can follow the convention of an existing feature.

<details>
<summary>Examples</summary>

* [OmniRouter](./omni-router.md) achieves omni-modality through standard single-modality models being executed via standard Open AI API tool calls. Developers can add it to existing agents without refactoring.
* `lemonade config` displays the service configuration and `lemonade config set` modifies it. Likewise, `lemonade backends` displays backend status and `lemonade backends install` modifies it. People who used one command can guess how the other works.

</details>

### Automate the Documentation Away

The best way to understand if a feature is sufficiently simple is to comprehensively document it. Is the document short or long? Is the feature straightforward to use, or is the document full of conditionals and gotchas?

Long, complex documentation means the feature is not sufficiently automated. Keep iterating until the document is simultaneously comprehensive *and* concise.

<details>
<summary>Example</summary>

* Problem: we had an "add a model" feature that required users to fill out a form with a name, checkpoint, variant ID, backend, and a bunch of labels like "vision", "embedding", etc. Users were rightfully confused.
* Document: We wrote a document that turned out to be multiple pages long. Not many people read it and confusion continued.
* Automate: Use the Hugging Face API and some automation to fill out the form automatically. Now users only have to provide a checkpoint, and the documentation is reduced to "just run `lemonade pull CHECKPOINT`".

</details>

### User Error is a Bug

You aren't holding it wrong, we just aren't done polishing the UI, UX, and docs yet.

<details>
<summary>Example</summary>

* If people are doing `lemonade pull https://huggingface.co/CHECKPOINT` instead of `lemonade pull CHECKPOINT` and hitting an error, just add support for the former.

</details>

### Backends are Fungible

The local AI backend ecosystem is heavily fragmented and each solution is rapidly advancing in its own unique way. Lemonade should provide maximum choice to users and builders while abstracting away the complexity of offering these choices.

<details>
<summary>Examples</summary>

* Lemonade should not promote any backend above any other.
* Migrating from backend A on operating system X to backend B on operating system Y should be 1 line of code / 1 click in the GUI.

</details>

### Design for Agility

People use Lemonade to access the bleeding edge of AI. The latest models and features are ideally available day0 and making people wait is a bug. New features should be designed in a way that accelerates future development and deployment.

<details>
<summary>Example</summary>

1. `lemonade-sdk/llamacpp-rocm` builds fresh artifacts every day.
2. A GitHub action automatically validates and merges llamacpp-rocm upgrades into Lemonade weekly.
3. Users can `lemonade config set llamacpp.rocm_bin="latest"` to get new llamacpp without waiting for new Lemonade, if weekly is too slow for them.

</details>

### The Customer is Right

Lemonade should be sufficiently flexible that you can use it the way you want. We just have to find a way to support you that respects the tenets above.

<details>
<summary>Example</summary>

* People wanted to use Lemonade alongside other LLM tools without duplicating model files. We added `lemonade config set extra_models_dir=DIR` to import already-downloaded models into Lemonade without re-downloading.

</details>
