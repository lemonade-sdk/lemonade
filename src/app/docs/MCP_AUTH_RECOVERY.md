# MCP admin authentication and recovery

## Problem fixed

GUI3 could reach the regular Lemonade API but fail on
`GET /internal/mcp/servers` after an API key was entered. The MCP error then
left the user without a reliable way to select a preset with no MCP or continue
the current chat without tools.

## Why it happened

Lemonade protects two endpoint classes with separate credentials:

- Regular `/api/*`, `/v0/*`, `/v1/*`, and `/mcp` requests use
  `LEMONADE_API_KEY`.
- `/internal/*`, including the MCP client-host administration endpoints, uses
  `LEMONADE_ADMIN_API_KEY`. When the admin variable is absent, the server falls
  back to its server-side `LEMONADE_API_KEY`.

The internal MCP API can launch local processes and is deliberately fail-closed.
When neither variable exists in the **lemond process environment**, it returns a
configuration error. Entering a key only in GUI3 changes the client request; it
does not configure or restart lemond.

A browser request carrying `Authorization` can also require a CORS preflight.
When that preflight is rejected, `fetch` exposes only a generic network error.
GUI3 now probes the server without authorization first and retries with admin
auth only after an explicit 401, yielding a useful server error whenever the
browser permits it.

## Client changes

- Added a separate, session-only MCP admin credential with regular API-key
  fallback.
- Every `/internal/mcp/*` mutation, tool discovery, and tool call now uses admin
  authentication explicitly.
- Added an **Admin API key** control under **Connect → MCP Gateway**.
- Added an always-available **No MCP** option to editable chat presets.
- Preset controls remain editable when external MCP discovery fails.
- Missing selected MCP servers remain visible so they can be deselected.
- An MCP setup failure no longer aborts the chat. GUI3 disables MCP for that
  chat, shows a warning, and continues the same request without tools.
- The external MCP smoke test now reads `LEMONADE_ADMIN_API_KEY`, falling back
  to `LEMONADE_API_KEY`, and prints an actionable 401/403 hint.

## Configure a packaged Linux service

```bash
sudo install -d -m 0750 /etc/lemonade/conf.d
sudo sh -c 'umask 077; cat > /etc/lemonade/conf.d/auth.conf <<EOF2
LEMONADE_API_KEY=choose-a-regular-key
LEMONADE_ADMIN_API_KEY=choose-a-strong-admin-key
EOF2'
sudo systemctl restart lemond.service
```

Some older packages use `lemonade-server.service` instead of `lemond.service`.
Use `systemctl list-units --type=service | grep -i lemon` to identify the
installed unit.

Then enter the matching admin key in GUI3. Leaving the admin field empty tells
GUI3 to reuse its regular API key.

## Verification

The following checks passed in the supplied source tree:

- isolated TypeScript/TSX transpilation for all changed source and test files;
- MCP API/admin credential contract test;
- newline-delimited stdio MCP mock handshake and tool call;
- JavaScript syntax checks for both MCP scripts;
- unified-diff whitespace check.

A full webpack/Playwright run was not possible in the packaging environment
because dependencies were not installed and network package retrieval was
unavailable. The browser recovery scenario is included in `tests/a11y.spec.ts`
for execution in the normal project environment.
