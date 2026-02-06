# How to use Lemonade LLMs with Claude Code

[Claude Code](https://docs.anthropic.com/en/docs/agents-and-tools/claude-code/overview) is an agentic coding tool from Anthropic that lives in your terminal. While it defaults to using Anthropic's models, you can configure it to use local models served by Lemonade via the LiteLLM Proxy.

This guide shows how to set up Claude Code to use Lemonade for inference and how to connect MCP servers.

## Prerequisites

- **Lemonade Server** installed and running (see [Getting Started](../README.md))
- **Claude Code** installed (`npm install -g @anthropic-ai/claude-code`)
- **LiteLLM** installed with proxy support (`pip install 'litellm[proxy]'`)

## Step 1: Configure LiteLLM Proxy

Create a `config.yaml` file to tell LiteLLM how to route requests to your local Lemonade server.

```yaml
model_list:
  # Define a model alias for Claude Code to use
  - model_name: lemonade-qwen
    litellm_params:
      # Use the 'openai/' prefix to tell LiteLLM to use the OpenAI protocol
      model: openai/Qwen2.5-Coder-7B-Instruct-GGUF
      # Point to your local Lemonade server
      api_base: http://localhost:8000/v1
      api_key: lemonade
```

> **⚠️ WARNING:** You **MUST** replace `Qwen2.5-Coder-7B-Instruct-GGUF` in the config above with the actual model name you have downloaded in Lemonade. Run `lemonade-server list` to see your available models.

## Step 2: Start LiteLLM Proxy

Run the proxy using your configuration file:

```bash
litellm --config config.yaml
```

The proxy will start on `http://0.0.0.0:4000`.

## Step 3: Configure Claude Code

Tell Claude Code to use your local LiteLLM proxy instead of Anthropic's API.

```bash
export ANTHROPIC_BASE_URL="http://0.0.0.0:4000"
# LiteLLM requires a master key if configured, or you can use a dummy value if not
export ANTHROPIC_AUTH_TOKEN="sk-1234" 
```

## Step 4: Run Claude Code

Now you can start Claude Code and specify the model you defined in `config.yaml`:

```bash
claude --model lemonade-qwen
```

Claude Code will now send prompts to LiteLLM, which forwards them to your local Lemonade server.

## Advanced: Using MCP Tools with Claude Code

Claude Code supports the [Model Context Protocol (MCP)](https://modelcontextprotocol.io/), allowing it to connect to external tools and data sources. You can manage these connections via LiteLLM.

### 1. Add MCP Servers to LiteLLM Config

Update your `config.yaml` to include MCP servers. For example, to add the GitHub MCP server:

```yaml
model_list:
  - model_name: lemonade-qwen
    litellm_params:
      model: openai/Qwen2.5-Coder-7B-Instruct-GGUF
      api_base: http://localhost:8000/v1
      api_key: lemonade

mcp_servers:
  github_mcp:
    url: "https://api.githubcopilot.com/mcp"
    auth_type: oauth2
    client_id: os.environ/GITHUB_OAUTH_CLIENT_ID
    client_secret: os.environ/GITHUB_OAUTH_CLIENT_SECRET
```

### 2. Connect Claude Code to the MCP Server

Once LiteLLM is running with the new config, you can add the MCP server to Claude Code:

```bash
claude mcp add --transport http litellm_proxy http://0.0.0.0:4000/github_mcp/mcp --header "Authorization: Bearer sk-1234"
```

### 3. Authenticate via Claude Code

1. Start Claude Code: `claude`
2. Type `/mcp` to manage connections.
3. Select `litellm_proxy`.
4. Follow the OAuth flow to authenticate (e.g., with GitHub).

Once authenticated, Claude Code can use the tools provided by the MCP server (like reading repositories or creating issues) while using your local Lemonade model for reasoning.

For more details on MCP support in LiteLLM, see the [LiteLLM MCP Documentation](https://docs.litellm.ai/docs/mcp).
