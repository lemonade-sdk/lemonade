# Ollama-Compatible API

Lemonade supports the [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), allowing applications built for Ollama to work with Lemonade without modification.

To enable auto-detection by Ollama-integrated apps, configure the server to use the Ollama default port `11434`. See [Server Configuration](../guide/configuration.md#settings-reference) for how to change the port.

| Endpoint | Status | Notes |
|----------|--------|-------|
| `POST /api/chat` | Supported | Streaming and non-streaming |
| `POST /api/generate` | Supported | Text completion + image generation |
| `GET /api/tags` | Supported | Lists downloaded models |
| `POST /api/show` | Supported | Model details |
| `DELETE /api/delete` | Supported | |
| `POST /api/pull` | Supported | Download with progress |
| `POST /api/embed` | Supported | New embeddings format |
| `POST /api/embeddings` | Supported | Legacy embeddings |
| `GET /api/ps` | Supported | Running models |
| `GET /api/version` | Supported | |
| `POST /api/create` | Not supported | Returns 501 |
| `POST /api/copy` | Not supported | Returns 501 |
| `POST /api/push` | Not supported | Returns 501 |
