# MCP actual serialization test decision

**Date:** 2026-07-30T10:51:42.821-06:00
**Owner:** Aaron

## Decision

Keep diagnostic security coverage attached to live MCP tool calls. Each diagnostic test parses the JSON content block returned by `/mcp`, applies recursive allowlist and forbidden-data checks to that response, and checks that ordinary relative paths and bare URLs are absent from the serialized payload rather than testing a handcrafted local dictionary.

The lifecycle fixture uses the existing downloaded tiny model, discovers an installable backend with an installed fallback, sets the configured backend plus `no_fetch_executables`, and restores the prior runtime configuration. It requires a structured MCP tool error and unchanged model/backend state, which demonstrates that the preflight returned before install or download management behavior.

## Validation

`python -m py_compile test\server_mcp.py`, `black --check test\server_mcp.py`, `git diff --check -- test\server_mcp.py`, and AST/static coverage checks passed. The live targeted command must be rerun after rebuilding the server because the current daemon returns `Unknown tool: lemonade_get_model_info` and therefore predates the diagnostic MCP tools.
