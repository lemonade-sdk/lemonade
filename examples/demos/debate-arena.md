## Debate Arena

Debate Arena is a single-file HTML/CSS/JS web app that pits up to 9 LLMs against each other to answer life's most important yes/no questions.

### Quick start instructions

1. Install Lemonade Server v9.0.8 or higher: https://github.com/lemonade-sdk/lemonade/releases
2. In a terminal: `lemonade-server serve --max-loaded-models 9`
3. Download https://github.com/lemonade-sdk/lemonade/blob/main/examples/demos/llm-debate.html and open it in your web browser
4. You can uncheck some models to save VRAM. Running all 9 requires ~32 GB.

### How it works

- All LLMs run at the same time, and the responses are streamed live to the UI.
- Web app connects to Lemonade at `http://localhost:8000`
    - Uses the `pull` endpoint to download the models
    - Uses the `load` endpoint to start 9 `llama-server` background processes, one each for Qwen, Jan, LFM, etc.
- The user’s prompt is sent to all 9 LLMs simultaneously for a hot take by starting 9 streaming `chat/completions` requests with Lemonade’s base URL.
    - The request with `model=Qwen...` gets routed to Qwen's `llama-server`, and so on.
- The 9 LLM responses go into a shared chat history, which is then sent back to the LLMs for reactions in debate phase 2.
- Process repeats until all 9 have voted, with escalating system prompts in each phase.
