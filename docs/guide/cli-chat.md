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
| `MODEL` (positional) | Model to chat with. If omitted, uses the currently loaded model, or prompts you to choose (one-shot requires a loaded model or `MODEL`). |
| `--system TEXT` | Set a system prompt for the session. Stored with the session; replaces a stored prompt when resuming. |
| `-p, --prompt TEXT` | Send one prompt, print the answer, and exit. |
| `-j, --json` | One-shot only: print one JSON object instead of text. |
| `--no-stream` | Wait for the full response instead of streaming token-by-token. |
| `--continue` | Open the most recently used saved session. |
| `--resume ID_OR_NAME` | Open a saved session by ID, a unique ID prefix, or name. |
| `--session NAME` | Open the session called `NAME`, creating it if it doesn't exist. Works with `-p`. |
| `--no-save` | Don't write anything to disk for this run. Saved sessions can still be opened, read-only. |

`--continue`, `--resume` and `--session` are mutually exclusive.

## Saved sessions

Interactive chats are saved automatically after every reply, so you can close the terminal and pick the conversation up later.

```bash
lemonade chat                       # new chat, saved as you go
lemonade chat --continue            # pick up the last one
lemonade chat --resume project      # by name, ID, or unique ID prefix
lemonade chat --session project     # open "project", or create it
lemonade chat --no-save             # nothing is written
```

A new session is only written once it holds a message, so opening and quitting leaves nothing behind. The title defaults to the first user message; `/rename` gives the session a name that `--resume` and `--session` accept.

**Where they live.** Each session is one JSON file in the `chat_sessions` folder of the Lemonade config directory (for example `~/.config/lemonade/chat_sessions/`). The file holds the whole transcript in plain text, unencrypted: ID, name, title, timestamps, model, system prompt, messages and token totals. On Linux and macOS the folder and files are readable only by you; on Windows they inherit the permissions of your profile folder. Delete a session with `/delete`, or remove its file. Use `--no-save` for chats you don't want on disk.

**Safe to use from several terminals.** Files are replaced atomically, so a crash or Ctrl-C never leaves a half-written session, and a session can be open for writing in only one `lemonade chat` at a time. A second interactive chat that tries to open it is refused; a one-shot call waits up to 30 seconds for its turn.

**What a resume does.**
- The stored model is used unless you pass `MODEL`, which replaces it and keeps the history.
- The stored system prompt is used unless you pass `--system`, which replaces it and keeps the history.
- A session written by a newer Lemonade is refused rather than rewritten.

### Sessions in scripts

Plain one-shot calls (`-p` or piped stdin) are **not** saved, so cron jobs and pipelines leave no transcripts. Name a session to keep one conversation going across calls:

```bash
lemonade chat MODEL --session review --system "You review diffs." -p "Start with this" < a.diff
lemonade chat --session review -p "Now check the tests"
lemonade chat --session review --no-save -p "Ask without recording it"
```

## Scripting (one-shot)

`-p` or piped stdin sends a single request and exits. With both, the prompt comes first, then stdin, in one user message.

```bash
lemonade chat MODEL -p "Say hi"
lemonade chat MODEL -p "Summarize" < log.txt
git diff | lemonade chat MODEL -p "Write a commit message"
lemonade chat MODEL -p "Say hi" --json
```

- Stdout is only the answer; progress and errors go to stderr. Never opens a picker.
- Non-terminal stdin is read to EOF. If it's an open pipe with nothing sent (e.g. `docker exec -i` without piping), it waits; close it or drop `-i`.
- `--json` prints `{"model", "message", "usage", "finish_reason"}` once, only on success.
- Exit codes: `0` ok, `1` request/load failure (a failed stream may leave partial output), `2` invalid input or options, `130` interrupted.

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

### Session commands

| Command | Effect |
|---|---|
| `/session` | Show the current session: ID, name, model, system prompt, token totals, file |
| `/history [text]` | Browse saved sessions, newest first. On a Linux or macOS terminal this is an interactive picker; elsewhere it prints a numbered list and asks for a number, ID or name. `text` limits the list to sessions whose name, title or messages contain it |
| `/new [name]` | Start a new session; the current one stays saved |
| `/rename <name>` | Name the current session (names are unique) |
| `/delete <id\|name\|#>` | Delete a saved session after a `y/N` confirmation. `#` is a row number from the last `/history` |
| `/export [file]` | Write the session to `file.md` or `file.json` (no name: `<session>.md`). Add `--format json\|md` to override the extension and `--force` to overwrite |
| `/import <file>` | Open a session from an exported `.md` or `.json` file. It is saved as a new session with a new ID; a taken name becomes `name-2`, and an unnamed session whose title is taken becomes `title (imported)` |
| `/system --keep <text>` | Change the system prompt and keep the conversation |

`/system <text>`, `/model <name>` and `/clear` keep their behavior of clearing the history; with autosave on, the cleared state is what stays saved. Use `/new` to start over without losing the old chat, and `/system --keep` to edit the prompt mid-conversation.

JSON exports are the session file itself. Markdown exports are readable as-is and import back without loss. If you edit a Markdown export, change the text inside a message block; text added after the last block becomes part of the last message. Keep the `<!-- lemonade:... -->` marker lines intact, since they separate the messages.

### Browsing sessions

`/history` opens a picker below the prompt. It lists your sessions with the date, message count, model and ID, and previews the first and last message of the highlighted one. It goes away when you pick something, so the chat stays an ordinary scrolling terminal.

| Key | Action |
|---|---|
| `Up` / `Down`, `PageUp` / `PageDown`, `Home` / `End` | Move the highlight |
| `Enter` | Open the highlighted session |
| `/` | Filter as you type, matching the name, title and message text; `Enter` keeps the filter, `Esc` clears it |
| `d` | Delete the highlighted session after a `y/N` confirmation |
| `r` | Rename the highlighted session |
| `Esc` or `q` | Close the picker (`Esc` first clears an active filter) |

`/history needle` starts with the filter already applied. The picker needs a terminal on Linux or macOS; with piped input, or on Windows, `/history` prints the numbered list instead.

### Key bindings

| Key | Action |
|---|---|
| `Enter` | Send the message (in multi-line mode: send when the line is blank) |
| `Up` / `Down` | Step through earlier messages and commands. On resume, your messages from the saved session are included |
| `Left` / `Right`, `Home` / `End` | Move the cursor (`Ctrl-Left` / `Ctrl-Right` jump by word; `Ctrl-A` / `Ctrl-E` also work) |
| `Backspace` / `Delete` | Delete a character; `Ctrl-W` deletes the previous word, `Ctrl-K` the rest of the line |
| `Esc` / `Ctrl-U` | Clear the current line |
| `Ctrl-C` | Stop a streaming response, cancel a multi-line buffer, or exit at an empty prompt |
| `Ctrl-D` / `Ctrl-Z` | Exit (EOF) |

The editing keys above apply on Linux and macOS terminals. The Windows console provides its own line editing and history (including Up and F7), so those keys behave as they do in any Windows console program.
