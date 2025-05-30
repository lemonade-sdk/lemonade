# Local LLM Server Concepts

This document gives background information about the main concepts for local LLM servers and 🍋Lemonade Server.

The intention is to answer these FAQs:

- [What is a Local Server?](#what-is-a-local-server)
- [What is a Local LLM Server?](#what-is-a-local-llm-server)
- [What is the OpenAI Standard?](#what-is-the-openai-standard)
- [How does the OpenAI Standard work?](#how-does-the-openai-standard-work)

## What is a Local Server?

First, let’s clarify what we mean by `server software`, as it’s sometimes confused with `server hardware`, which is the actual physical systems running in data centers.
- `Server software` refers to a process running on a computer that listens for and responds to requests initiated by `client software` (i.e., applications).
- `Server software` often runs on `server hardware`, but there are many examples of `server software` running on the same `client hardware` (laptop, desktop computer, tablet, or smartphone) as the `application`.

A `local server` is `server software` that runs on `client hardware`.

## What is a Local LLM Server?

Local LLM servers are an extremely popular way of deploying LLMs directly to `client hardware`. A few famous examples of local LLM servers include [Ollama](https://ollama.com/), [llama-cpp-server](https://github.com/ggml-org/llama.cpp/tree/master/tools/server), and [Docker Model Runner](https://docs.docker.com/model-runner/).

The local server process loads the LLM into memory and exposes it to client software for handling requests. Compared to integrating the LLM directly into the client software using C++ or Python APIs, this setup provides the following benefits:

| Benefit | Description |
|---------|-------------|
| Simplified integration | C++/Python APIs are typically framework- (e.g., llama.cpp, OGA, etc.) and/or device- (e.g., CPU, GPU, NPU, etc.) specific. Local LLM servers, on the other hand, facilitate conversing with the LLM at a high level that abstracts these details away (see [What is the OpenAI Standard?](#what-is-the-openai-standard)). |
| Sharing LLMs between applications | A single local LLM can take up a significant portion of system RAM. The local LLM server can share this LLM between multiple applications, rather than requiring each application to load its own LLM into RAM. |
| Separation of concerns | Installing and managing LLMs, enabling features like tool use and streaming generation, and building in fault tolerance can be tricky to implement. A local LLM server abstracts away this complexity, letting application developers stay focused on their app. |
| Cloud-to-client development | A common practice for LLM developers is to first develop their application using cloud LLMs, then switch to local LLMs later in development. Local and cloud LLM servers behave similarly from the application's perspective, which makes this transition seamless. |

## What is the OpenAI Standard?

All LLM servers (cloud or local) adhere to an application-program interface (API). This API lets the `application` make LLM requests to the `server software`.

While there are several popular LLM server APIs available in the LLM ecosystem, the [OpenAI API](https://platform.openai.com/docs/guides/text?api-mode=chat) has emerged as the industry standard because it is (at the time of this writing) the only API that meets these three criteria:
1. Dozens of popular LLM `servers` support OpenAI API.
1. Dozens of popular LLM `applications` support OpenAI API.
1. OpenAI API is broadly supported in both `local` and `cloud`.

Crucially, while OpenAI offers their own LLM API-as-a-cloud-service, their API standard is rigorously documented and available for other cloud and local servers to adopt.

## How does the OpenAI Standard Work?

In the OpenAI API standard, applications and servers communicate in the form of a multi-role conversation. There are three "roles" in this context: the "system", the "assistant", and the "user".

| Role      | Description |
|-----------|-------------|
| System    | Allows the application to provide instructions to the LLM, such as defining its persona, what tools are available to it, what tasks it is supposed to help with or not help with, etc. |
| Assistant | Messages sent from the LLM to the application. |
| User      | Messages sent from the application to the LLM. Often these messages are written by the application's end-user. |

OpenAI also provides [convenient libraries](https://platform.openai.com/docs/libraries/python-library#install-an-official-sdk) in JavaScript, Python, .Net, Java, and Go to help application and server developers adhere to the standard.

For example, the following Python code demonstrates how an application can request an LLM response from the Lemonade Server:

```python
# Client library provided by OpenAI to automate request
# and response processing with the server
from openai import OpenAI

# The base_url points to an LLM server, which can either be
# local (localhost address) or cloud-based (web address)
base_url = f"http://localhost:8000/api/v1"

# The `client` instance here provides APIs to request
# LLM invocations from the server
client = OpenAI(
    base_url=base_url,
    api_key="lemonade",  # required, but unused in Lemonade
)

# The `messages` list provides the history of messages from
# the system, assistant, and user roles
messages = [
    {"role":"system", "content":"You are a helpful assistant."},
    {"role":"user", "content":"Hi, how are you?"},
]

# This is the API call that sends the `messages` history to
# the server's specific LLM `model`
# It returns a `completion`, which is OpenAI's way of referring
# to the LLM's reponse to the messages
completion = client.chat.completions.create(
    model="Llama-3.1-8B-Instruct-Hybrid",
    messages=messages,
)

# This code gets the LLM's response from the `completion`
# and prints it to the screen
reponse = completion.choices[0].message.content
print(response)
```

The Python above will work with Lemonade Server, along with a variety of other cloud and local LLM servers, just by changing the `base_url`, `api_key`, and `model` as needed. This example demonstrates that details like deployment location (local vs. cloud), hardware type (GPU vs. NPU), and backend implementation (OGA vs. llama.cpp), etc. are hidden behind a unified interface.

<!--Copyright (c) 2025 AMD-->
