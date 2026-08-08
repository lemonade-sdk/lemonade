# GUI3 MCP integration

## Architecture

GUI3 treats every chat tool provider as an MCP selection stored by preset.

- `lemonade` is a built-in MCP provider. It preserves the existing model-management tools and adds image generation/editing, audio generation, TTS, transcription, and 3D generation.
- External MCP servers are managed by lemond's `/internal/mcp/*` client-host API. Streamable HTTP is the default connection type; local `stdio` processes remain available as an advanced option. These control endpoints are fail-closed and require a server-side admin/API key.
- A preset stores `mcp_server_ids` and can select zero to four providers. Legacy presets migrate deterministically: `tools_enabled: true` becomes `['lemonade']`; `false` becomes `[]`.
- External tool names use the server-provided `chat_name`, preventing collisions when several servers expose a tool with the same raw name.

## 3D flow

`generate_3d` accepts either an image or a prompt. With an image it calls image-to-3D directly. With prompt only it first generates a centered 1024x1024 reference image, then loads a 3D backend and reconstructs the generated image. The chat result contains both the reference image and the GLB artifact.

## Configure an external server

Open **Settings → MCP Gateway → External MCP servers** and choose a connection type:

- **HTTP endpoint** (recommended): enter the external application's Streamable HTTP endpoint, for example `http://127.0.0.1:3000/mcp`. HTTPS is required for non-local endpoints unless insecure HTTP is explicitly enabled. Optional bearer authentication uses an environment-variable reference, never a raw token.
- **Local process** (advanced): enter the executable in **Command** and one argument per line. Lemonade starts and supervises the process on the lemond host, not in the browser.

Environment values must be references such as `GITHUB_TOKEN=${GITHUB_TOKEN}`. Raw secret values are rejected and are never persisted. The referenced variable must already exist in the lemond process environment. Use **Test connection** before saving to validate initialization and tool discovery without persisting the draft.

After saving, open Chat and choose **+ → Tools → External MCP servers**. Select the server, then optionally select individual tools. Up to four providers can be active. Turning off **Tools for this chat** clears runtime use without deleting the saved server configuration.

## Admin authentication for external MCP management

`/internal/mcp/*` can connect to network endpoints and launch local processes, so current Lemonade builds deliberately reject MCP administration unless the **lemond process itself** was started with one of these environment variables:

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

In **Settings → MCP Gateway → External MCP servers**, enter the matching **Admin API key**. Leave it empty to reuse the normal API key. The explicit admin key is session-only in GUI3.

If the server has neither environment variable, the MCP panel remains usable but displays a configuration error. Chat remains usable: external server administration is blocked, while built-in Lemonade tools can still be selected or tools can be disabled for the chat.

## Automated external MCP smoke test

Start a current lemond build containing this client-host implementation, then run from the GUI directory:

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

The smoke test validates both supported transports end to end:

- creates an ephemeral Streamable HTTP MCP endpoint, tests the unsaved draft, persists it, verifies session/version headers, consumes a deliberately open SSE response, discovers `echo`, and calls it;
- starts the existing Node stdio mock, discovers its `echo` tool, and calls it;
- verifies that both external tools receive collision-safe namespaced chat names;
- disconnects and removes every temporary configuration.
