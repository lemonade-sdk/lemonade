# Getting Started with Lemonade Server

🍋 Lemonade Server is a server interface that uses the standard Open AI API, allowing applications to integrate with local LLMs. This means that you can easily replace cloud-based LLMs with private and free LLMs that run locally on your own PC's NPU and GPU.

Lemonade Server is available as a standalone tool with a [one-click Windows GUI installer](https://github.com/lemonade-sdk/lemonade/releases/latest/download/Lemonade_Server_Installer.exe).

No matter how the server is launched, you can configure its host, port, log
level, Llama.cpp backend, and context size by setting the environment variables
`LEMONADE_HOST`, `LEMONADE_PORT`, `LEMONADE_LOG_LEVEL`, `LEMONADE_LLAMACPP`, and
`LEMONADE_CTX_SIZE`.

Once you've installed, we recommend checking out these resources:

| Documentation | Description |
|---------------|-------------|
| [Supported Applications](./apps/README.md) | Explore applications that work out-of-the-box with Lemonade Server. |
| [Lemonade Server Concepts](./concepts.md) | Background knowledge about local LLM servers and the OpenAI standard. |
| [`lemonade-server` CLI Guide](./lemonade-server-cli.md) | Learn how to manage the server process and install new models using the command-line interface. |
| [Models List](./server_models.md) | Browse a curated set of LLMs available for serving. |
| [Server Spec](./server_spec.md) | Review all supported OpenAI-compatible and Lemonade-specific API endpoints. |
| [Integration Guide](./server_integration.md) | Step-by-step instructions for integrating Lemonade Server into your own applications. |

> Note: if you want to develop Lemonade Server itself, you can [install from source](https://lemonade-server.ai/install_options.html).

## Integrate Lemonade Server with Your Application

Since Lemonade Server implements the standard OpenAI API specification, you can use any OpenAI-compatible client library by configuring it to use `http://localhost:8000/api/v1` as the base URL. A table containing official and popular OpenAI clients on different languages is shown below.

Feel free to pick and choose your preferred language.


| Python | C++ | Java | C# | Node.js | Go | Ruby | Rust | PHP |
|--------|-----|------|----|---------|----|-------|------|-----|
| [openai-python](https://github.com/openai/openai-python) | [openai-cpp](https://github.com/olrea/openai-cpp) | [openai-java](https://github.com/openai/openai-java) | [openai-dotnet](https://github.com/openai/openai-dotnet) | [openai-node](https://github.com/openai/openai-node) | [go-openai](https://github.com/sashabaranov/go-openai) | [ruby-openai](https://github.com/alexrudall/ruby-openai) | [async-openai](https://github.com/64bit/async-openai) | [openai-php](https://github.com/openai-php/client) |


### Python Client Example
```python
from openai import OpenAI

# Initialize the client to use Lemonade Server
client = OpenAI(
    base_url="http://localhost:8000/api/v1",
    api_key="lemonade"  # required but unused
)

# Create a chat completion
completion = client.chat.completions.create(
    model="Llama-3.2-1B-Instruct-Hybrid",  # or any other available model
    messages=[
        {"role": "user", "content": "What is the capital of France?"}
    ]
)

# Print the response
print(completion.choices[0].message.content)
```

For more detailed integration instructions, see the [Integration Guide](./server_integration.md).


<!--Copyright (c) 2025 AMD-->