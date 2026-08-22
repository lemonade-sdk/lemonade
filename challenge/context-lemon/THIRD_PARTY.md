# Third-party software

The Lemonade Context Engine itself is MIT-licensed (see `LICENSE`).

## Direct dependencies

Licences below were read from each crate's own `Cargo.toml` in the local registry, not
from memory. All are permissive; none are copyleft.

| Crate | Licence | Used for |
|---|---|---|
| `tauri` | Apache-2.0 OR MIT | Application shell, tray, window |
| `tauri-plugin-dialog` | Apache-2.0 OR MIT | Native folder picker |
| `tauri-plugin-autostart` | Apache-2.0 OR MIT | Launch-at-login registration |
| `tauri-plugin-opener` | Apache-2.0 OR MIT | Opening cited files |
| `serde` | MIT OR Apache-2.0 | Serialisation derives |
| `serde_json` | MIT OR Apache-2.0 | Config file, API payloads |
| `bincode` | MIT | On-disk index encoding |
| `reqwest` | MIT OR Apache-2.0 | HTTP client for the Lemonade API |
| `ignore` | Unlicense OR MIT | gitignore-aware directory walking |
| `notify` | CC0-1.0 | Filesystem change events for live re-indexing |
| `dirs` | MIT OR Apache-2.0 | Locating `%APPDATA%` |
| `tokio` (dev) | MIT | Async test harness |

All are permissive. `notify` is CC0-1.0 — a public-domain dedication, i.e. even less
restrictive than MIT, but worth naming precisely rather than lumping it in with the
MIT/Apache majority.

The full resolved tree is **519 crates**. Regenerate the complete list with:

```powershell
cargo install cargo-about
cargo about generate about.hbs
```

Note that `walkdir` and `uuid` appear in `Cargo.lock` but are **not** direct
dependencies — they arrive transitively through `ignore` and `tauri` respectively.

## Frontend

TypeScript + Vite, no runtime UI framework and no bundled web fonts. The application
ships no CDN references and makes no network requests other than to the local Lemonade
server.

## Not redistributed

- **Lemonade Server** — installed separately by the user.
- **Models** (`nomic-embed-text-v1-GGUF`, `Qwen3-0.6B-GGUF`) — pulled by the user
  through Lemonade. Their own licences apply and are not restated here.

## Bundled content

`sample/` contains four short Markdown files describing "Project Nightingale" at
"Aeroflux Systems". Both are fictional, written for this project, and exist so the app
has a corpus with checkable facts to demonstrate retrieval against on first run. Any
resemblance to a real product or company is coincidental.
