# Lemonade Endpoints Spec

The Lemonade HTTP service provides a wide array of standards-compliant and custom endpoints.

Our design philosophy is:

1. Ensure that Lemonade works out-of-box with all popular local AI apps.
2. Prioritize using a pre-existing standard for functionality whenever possible.
3. Add sufficient custom functionality to enable developers to build highly polished experiences.

This spec details all supported endpoints. It is organized into pages that correspond to  which organization (OpenAI, Ollama, Lemonade, etc.) defined the endpoints.

| API | Description |
|-----|-------------|
| [OpenAI-Compatible API](./openai.md) | Start here for the main API surface used by most SDKs and clients. |
| [Ollama-Compatible API](./ollama.md) | Use this if your client expects Ollama-style behavior and routes. |
| [Anthropic-Compatible API](./anthropic.md) | Use this for clients built around Anthropic's message format. |
| [MCP Gateway](./mcp.md) | Use this to expose Lemonade as a Model Context Protocol server (POST /mcp). |
| [llama.cpp-Specific API](./llamacpp.md) | Reference for llama.cpp-specific compatibility and conventions. |
| [Lemonade-Specific API](./lemonade.md) | Local-first API for managing lifecycle, configuration, backends, etc. |
| [Router API](./router.md) | How `collection.router` models route requests, and how to test a routing policy. |
| [Internal API](./internal.md) | Control endpoints for clients bundled with the server, secured by `LEMONADE_ADMIN_API_KEY`. |

A running server also serves these pages itself: [`GET /v1/docs`](./lemonade.md#get-v1docs) lists what it has and [`GET /v1/docs/{page}`](./lemonade.md#get-v1docspage) returns one, so the reference always matches the installed version and stays available offline.

## Authentication

When `LEMONADE_API_KEY` is set, clients send it in the `Authorization` header as a Bearer token:

```bash
curl http://localhost:13305/v1/models -H "Authorization: Bearer $LEMONADE_API_KEY"
```

Each route says whether it needs a key. The internal routes take `LEMONADE_ADMIN_API_KEY` when it is set, and `LEMONADE_API_KEY` otherwise; the admin key also works on every other route. Lemonade reads no other header, so an Anthropic SDK client passes the key as `auth_token`, not `api_key`. The WebSocket APIs take the key as an `api_key` query parameter instead. See [API Key and Security](../guide/configuration/README.md#api-key-and-security) for every combination of the two keys.

## Errors

Each route lists the errors it answers with in its Errors table. These come from the server before a request reaches a route, so no route lists them:

| Status | Body | When |
|--------|------|------|
| `401` | `{"error": "Invalid or missing API key"}`, or `"Invalid or missing admin API key"` for an internal route | The request lacks the key its route needs |
| `403` | `{"error": "Origin not allowed"}` | The `Origin` header names an origin that [`allowed_origins`](../guide/configuration/README.md) does not allow |
| `404` | `{"error": {"message": "The requested endpoint does not exist", "type": "not_found", "path"}}` | No route matches the path |
| `405` | `{"error": "Method Not Allowed. Use POST for this endpoint"}` | A `GET` reaches a `POST`-only route under `/v0/`, `/v1/`, `/api/v0/` or `/api/v1/` |
