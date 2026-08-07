# Chat in your terminal

Lemonade ships with a built-in chat REPL so you can talk to a local model without leaving the terminal, no browser, no editor integration, no extra dependencies.

```text
lemonade chat
```

That's it. If no model is loaded, you'll be prompted to pick one. If a model is already loaded server-side, it's used automatically.

## Launching

Two equivalent entry points:

```bash
# Open the REPL, optionally with a model
lemonade chat
lemonade chat Qwen3-1.7B-Hybrid

# Or piggyback on the existing run command
lemonade run Qwen3-1.7B-Hybrid --chat-cli
```

`lemonade chat` is the recommended form. `lemonade run --chat-cli` exists so muscle memory from other tools still works.

### Options

| Option | Description |
|---|---|
| `MODEL` (positional) | Model to chat with. If omitted, uses the currently loaded model, or prompts you to choose. |
| `--system TEXT` | Set a system prompt for the session. |
| `--no-stream` | Wait for the full response instead of streaming token-by-token. Useful when piping output. |

## Using the REPL

Once you're in, the prompt is just `>`. Anything you type that doesn't start with `/` is sent as a user message. Streaming output flows back inline.

```text
  ─── Qwen3-1.7B-Hybrid ───────────────────────────────────  ? /help for shortcuts
  Clear line: Esc / Ctrl-U · Stop response or exit: Ctrl-C

  >
```

Type `/help` at any time for the full list of slash commands (switching models, toggling multi-line input, viewing stats, listing or unloading models, etc.).

### Reasoning models

If the active model emits a thinking trace (Qwen 3, Phi-4-mini-reasoning, DeepSeek-R1, …), the REPL renders it above the answer:

The reasoning trace is display-only, it isn't kept in the chat history sent back on the next turn.

Use `/think` to control whether the model reasons at all:

| Command | Effect |
|---|---|
| `/think` | Show the current reasoning mode |
| `/think on` | Ask the model to reason |
| `/think off` | Suppress the thinking trace (injects `/no_think` on supported models) |
| `/think default` | Stop overriding, let the model do whatever it does by default |

### Key bindings

| Key | Action |
|---|---|
| `Enter` | Send the message (in multi-line mode: send when the line is blank) |
| `Esc` / `Ctrl-U` | Clear the current line |
| `Ctrl-C` | Stop a streaming response, cancel a multi-line buffer, or exit at an empty prompt |
| `Ctrl-D` / `Ctrl-Z` | Exit (EOF) |
