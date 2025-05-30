# Lemonade Server Examples

Many applications today utilize OpenAI models like ChatGPT through APIs such as:

`POST https://api.openai.com/v1/chat/completions`

This API call triggers the ChatGPT model to generate responses for a chat. With Lemonade Server, we are replacing the OpenAI endpoint with a local LLM. The new API call becomes:

`POST http://localhost:8000/api/v1/chat/completions`

This allows the same application to leverage local LLMs instead of relying on OpenAI's cloud-based models. The guides in this folder show how to connect Lemonade Server to popular applications to enable local LLM execution. To run these examples, you'll need a Windows PC.

## 🎥 Video Tutorials

<div id="yt-carousel" data-videos='[
  {"id": "PXNTDZREJ_A", "title": "Open WebUI Demo"},
  {"id": "JecpotOZ6qo", "title": "Microsoft AI Toolkit Demo"},
  {"id": "bP_MZnDpbUc", "title": "Continue Coding Assistant"},
  {"id": "_PORHv_-atI", "title": "GAIA"}
]'></div>

<div class="hide-in-mkdocs">

Links to the video tutorials available are provided in the third column of the following table.

</div>

| App                 | Guide                                                                                               | Video                                                                                     |
|---------------------|-----------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------|
| [Open WebUI](https://github.com/open-webui/open-webui)         | [How to chat with Lemonade LLMs in Open WebUI](https://ryzenai.docs.amd.com/en/latest/llm/server_interface.html#open-webui-demo)   | [Watch Demo](https://www.youtube.com/watch?v=PXNTDZREJ_A)                                 |
| [Continue](https://www.continue.dev/)   | [How to use Lemonade LLMs as a coding assistant in Continue](./continue.md)                                          | [Watch Demo](https://youtu.be/bP_MZnDpbUc?si=hRhLbLEV6V_OGlUt)                            |
| [Microsoft AI Toolkit](https://learn.microsoft.com/en-us/windows/ai/toolkit/)   | [Experimenting with Lemonade LLMs in VS Code using Microsoft's AI Toolkit](./ai-toolkit.md)                                          | [Watch Demo](https://youtu.be/JecpotOZ6qo?si=WxWVQhUBCJQgE6vX)                            |
| [GAIA](https://github.com/amd/gaia)   | [An application for running LLMs locally, includes a ChatBot, YouTube Agent, and more](https://github.com/amd/gaia?tab=readme-ov-file#getting-started-guide) | [Watch Demo](https://youtu.be/_PORHv_-atI?si=EYQjmrRQ6Zy2H0ek)                            |
| [CodeGPT](https://codegpt.co/)   | [How to use Lemonade LLMs as a coding assistant in CodeGPT](./codeGPT.md)                                          | _coming soon_                                                                             |
| [MindCraft](https://github.com/kolbytn/mindcraft) | [How to use Lemonade LLMs as a Minecraft agent](./mindcraft.md) | _coming soon_                                                                             |
| [wut](https://github.com/shobrook/wut)   | [Terminal assistant that uses Lemonade LLMs to explain errors](./wut.md)                                          | _coming soon_                                                                             |
| [AnythingLLM](https://anythingllm.com/) | [Running agents locally with Lemonade and AnythingLLM](./anythingLLM.md) | _coming soon_                                                                             |
| [lm-eval](https://github.com/EleutherAI/lm-evaluation-harness)   | [A unified framework to test generative language models on a large number of different evaluation tasks.](./lm-eval.md)              | _coming soon_                                                                             |
| [PEEL](https://github.com/lemonade-apps/peel)     | [Using Local LLMs in Windows PowerShell](https://github.com/lemonade-apps/peel?tab=readme-ov-file#installation)                   | _coming soon_                                                                             |

## 📦 Looking for Installation Help?

To set up Lemonade Server, check out the [Lemonade Server guide](../README.md) for installation instructions and the [server spec](../server_spec.md) to learn more about the functionality. For more information about 🍋 Lemonade SDK, see the [Lemonade SDK README](../README.md).

## 🛠️ Support

If you encounter any issues or have questions, feel free to:

- File an issue on our [GitHub Issues page](https://github.com/lemonade-sdk/lemonade/issues).
- Email us at [lemonade@amd.com](mailto:lemonade@amd.com).

## 💡 Want to Add an Example?

If you've connected Lemonade to a new application, feel free to contribute a guide by following our contribution guide found [here](../../contribute.md) or let us know at [lemonade@amd.com](mailto:lemonade@amd.com).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
