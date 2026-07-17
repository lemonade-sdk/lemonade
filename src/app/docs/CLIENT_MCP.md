# GUI3 MCP integration

## Architecture

GUI3 treats every chat tool provider as an MCP selection stored by preset.

- `lemonade` is a built-in MCP provider. It preserves the existing model-management tools and adds image generation/editing, audio generation, TTS, transcription, and 3D generation.
- External MCP servers are managed by lemond's `/internal/mcp/*` client-host API and currently use `stdio` transport. These control endpoints are fail-closed and require a server-side admin/API key.
- A preset stores `mcp_server_ids` and can select zero to four providers. Legacy presets migrate deterministically: `tools_enabled: true` becomes `['lemonade']`; `false` becomes `[]`.
- External tool names use the server-provided `chat_name`, preventing collisions when several servers expose a tool with the same raw name.

## 3D flow

`generate_3d` accepts either an image or a prompt. With an image it calls image-to-3D directly. With prompt only it first generates a centered 1024x1024 reference image, then loads a 3D backend and reconstructs the generated image. The chat result contains both the reference image and the GLB artifact.

## Configure an external server

Open **Connect → Model Context Protocol → External MCP servers**. Enter the executable in **Command** and one argument per line. Commands run on the lemond host, not in the browser.

Environment values must be references such as `GITHUB_TOKEN=${GITHUB_TOKEN}`. Raw secret values are rejected and are never persisted. The referenced variable must already exist in the lemond process environment.

After saving, open a chat-capable preset and select the server under **MCP**. Up to four servers can be selected. **No MCP** is always available and clears every selection even if external MCP discovery is currently failing.

## Admin authentication for external MCP management

`/internal/mcp/*` can launch local processes, so current Lemonade builds deliberately reject MCP administration unless the **lemond process itself** was started with one of these environment variables:

For a standalone process:

```bash
export LEMONADE_ADMIN_API_KEY='choose-a-strong-admin-key'
# Optional regular API key for non-internal endpoints:
export LEMONADE_API_KEY='choose-a-regular-api-key'
lemond
```

For the packaged Linux systemd service, place the values in a protected file under `/etc/lemonade/conf.d/` and restart the installed Lemonade unit:

```bash
sudo install -d -m 0750 /etc/lemonade/conf.d
sudo sh -c 'umask 077; cat > /etc/lemonade/conf.d/auth.conf <<EOF
LEMONADE_API_KEY=choose-a-regular-api-key
LEMONADE_ADMIN_API_KEY=choose-a-strong-admin-key
EOF'
sudo systemctl restart lemond.service  # some older packages use lemonade-server.service
```

When `LEMONADE_ADMIN_API_KEY` is not set, Lemonade falls back to the server-side `LEMONADE_API_KEY`. Setting an API key only in GUI3 configures the client request; it does **not** configure or restart the server.

In **Connect → MCP Gateway → External MCP servers**, enter the matching **Admin API key**. Leave it empty to reuse the normal API key. The explicit admin key is session-only in GUI3.

If the server has neither environment variable, the MCP panel remains usable but displays a configuration error. Presets are not locked: you can still select **No MCP** or the built-in **Lemonade** provider.

## Automated external MCP smoke test

Start a current lemond build containing commit `d1e42bab` or later, then run from this GUI directory:

```bash
npm run test:mcp-external
```

Optional settings:

```bash
LEMONADE_BASE_URL=http://127.0.0.1:13305 \
LEMONADE_API_KEY=your-regular-key \
LEMONADE_ADMIN_API_KEY=your-admin-key \
npm run test:mcp-external
```

The test registers a temporary Node stdio MCP server, initializes it, discovers its `echo` tool, calls it with `hello-mcp`, verifies the namespaced chat tool name and result, then disconnects and removes the temporary configuration.
