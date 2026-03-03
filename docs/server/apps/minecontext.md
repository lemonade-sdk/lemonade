# MineContext

[MineContext](https://github.com/volcengine/MineContext) is an open-source, proactive context-aware AI assistant. This document explains how to configure MineContext to use local AI models powered by AMD NPU (via Lemonade Server), enabling AI chat, screen monitoring, and work summarization capabilities—all data is processed locally, ensuring complete privacy.

There are a few things to note on this integration:

* **Privacy-First Architecture**: MineContext stores all data locally on your device. Combined with Lemonade Server's local inference, your data never leaves your machine.

* **NPU Acceleration solution**: This integration leverages AMD Ryzen AI NPU for VLM inference.

* **Multi-Model Support**: MineContext requires a Vision-Language Model for screen understanding and an Embedding model for context retrieval. Lemonade Server supports both NPU and GPU backends, allowing flexible deployment based on your hardware.

* **Universal Compatibility**: While this guide focuses on NPU-optimized configuration, Lemonade Server also supports GPU-only deployment using Vulkan or ROCm backends.

* **Hardware Requirements and Current Status**: This integration is still in its early stages. We encourage you to test it and share any issues you encounter. For the best NPU experience, the minimum configuration is Ryzen AI 300 Series with 32GB RAM. We recommend using a Strix Halo PC with 64GB or more RAM.


## Prerequisites

- **Lemonade Server**: Install Lemonade Server using the [Getting Started Guide](https://lemonade-server.ai/docs/server/).
- **Server running**: Ensure Lemonade Server is running on `http://localhost:8000`
- **Models installed**: MineContext requires two types of models:
  - **Vision-Language Model**: For screen capture understanding and AI chat. We recommend `Qwen3-VL-4B-Instruct-FLM`, which uses FLM server backend, optimized for AMD NPU.
  - **Embedding Model**: For context retrieval and similarity search. We recommend `Qwen3-Embedding-0.6B-GGUF`, which uses llama-server backend, running on GPU.
- **MineContext**: Download the appropriate version from [MineContext Releases](https://github.com/volcengine/MineContext/releases) (v0.1.7 recommended).


## Installation

### Launch Lemonade Server with Optimized Settings

For optimal MineContext performance, launch Lemonade Server with extended context size and NPU concurrency tuning:

```bash
lemonade-server serve --ctx-size 32768 --flm-args "-s 32 -q 32"
```

**Parameter explanation:**

You can adjust `--ctx-size` based on your available memory. Larger context windows can process more screen history but require more memory.

**Alternative: GPU-Only Configuration**

If you prefer to run all models on GPU (without NPU), you can use GGUF versions of VLM models.

### Installing MineContext

1. Refer to [MineContext](https://github.com/volcengine/MineContext/blob/main/README.md) for local installation instructions:
   - **Windows**: `MineContext-x.x.x-setup.exe`

2. Run the installer to complete installation. On first launch, the application will set up its backend environment (approximately 2 minutes).


## Configuring MineContext

When first launching MineContext, you'll need to configure it to connect to Lemonade Server.

1. Open MineContext and navigate to **Settings**, **Model platform**: Select `Custom`.

2. Configure the **VLM Model** settings:
   - **URL**: `http://localhost:8000/api/v1`
   - **Model**: `Qwen3-VL-4B-Instruct-FLM`
   - **API Key**: Enter any character (e.g., `-`), as Lemonade Server doesn't require authentication in local mode

3. Configure the **Embedding Model** settings:
   - **URL**: `http://localhost:8000/api/v1`
   - **Model**: `Qwen3-Embedding-0.6B-GGUF`
   - **API Key**: Enter any character (e.g., `-`)

4. Click **Save** to apply the configuration.

<div align="center">
  <br><em>MineContext Model Configuration Interface</em></br>
  <img src="https://github.com/user-attachments/assets/08dadbf4-f235-4a5b-949a-dfe6c3f3e708" alt="MineContext Model Configuration Interface" width="700"/>
</div>


## Using MineContext

### AI Chat

Chat with AI using your captured screen context:

1. Navigate to **Chat with AI**.

2. Enter your question in the chat box. MineContext will provide you with the corresponding answer:

<div align="center">
  <br><em>AI Chat Interface</em></br>
  <img src="https://github.com/user-attachments/assets/18ba5d37-a304-478f-8910-d4f7f01bd76f" alt="AI Chat Interface" width="700"/>
</div>

### Enable Screen Monitor

Screen Monitor is MineContext's core feature that captures and analyzes your screen content.

1. Navigate to the **Screen Monitor** section.

2. On first use, grant screen recording permissions when prompted.

3. After granting permissions, restart the application for changes to take effect.

4. After restart, configure your screen capture area in **Settings**, then click **Start Recording**.

5. Once recording starts, MineContext will analyze your screen content in the background using the local VLM model. This context is used for AI chat and work summaries.

<div align="center">
  <br><em>Screen Monitor Feature</em></br>
  <img src="https://github.com/user-attachments/assets/f3386a5e-e2a0-42a5-82c9-f258cde72a8c" alt="Screen Monitor Feature" width="700"/>
</div>

### Work Summary

MineContext automatically generates insights based on your screen activity:

1. From the main page, view auto-generated content:
   - **Daily Summary**: Overview of your daily activities
   - **Todo Items**: Automatically extracted action items
   - **Activity Report**: Detailed activity records

<div align="center">
  <br><em>Work Summary and Todo Items Interface</em></br>
  <img src="https://github.com/user-attachments/assets/f6f3c764-9af6-43d7-a4c1-562f2f6f4182" alt="Work Summary and Todo Items Interface" width="700"/>
</div>

2. These summaries update automatically based on your screen monitoring data—no manual input required.


### Backend Debugging

MineContext provides a web-based debugging interface at `http://localhost:1733`:

1. **Token Usage**: Monitor model consumption and API calls
2. **Task Intervals**: Configure screenshot and summary generation frequency
3. **System Prompts**: Customize AI behavior with custom prompts

<div align="center">
  <br><em>Backend Debugging Interface</em></br>
  <img src="https://github.com/user-attachments/assets/36f847bc-8e3a-4c33-9578-4dd189349e73" alt="Backend Debugging Interface" width="700"/>
</div>


## Common Issues

* **Connection refused error**: Ensure Lemonade Server is running. Check with `lemonade-server status` or verify the server is accessible at `http://localhost:8000`.

* **Model loading slow on first use**: Initial model loading requires loading weights into memory (VLM to NPU, Embedding to GPU). Subsequent uses will be faster as models remain cached.

* **Context window exceeded**: If conversations are being truncated, increase the context size:
  ```bash
  lemonade-server serve --ctx-size 65536 --flm-args "-s 32 -q 32"
  ```

* **Out of memory errors**: Running both VLM and Embedding models requires sufficient RAM. Try reducing the `--ctx-size` value:
  ```bash
  lemonade-server serve --ctx-size 16384 --flm-args "-s 32 -q 32"
  ```

* **Screen recording not working**: Ensure you've granted screen recording permissions and restarted the application after granting them.


## Known Issues

* **Embedding validation Error** (v0.1.8): `Embedding validation failed: 'OpenAI' object has no attribute 'multimodal_embeddings'`

* **Validation Error**: `Data validation failed because a list was passed instead of the required string`


## Resources

* [MineContext GitHub](https://github.com/volcengine/MineContext)
* [Lemonade Server](https://lemonade-server.ai)
* [FastFlowLM](https://fastflowlm.com/)
