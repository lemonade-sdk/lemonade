# Quick Start Guide

Get up and running with Ryzen AI LLM Server in minutes!

## Step 1: Prerequisites

Ensure you have:
- ✓ Ryzen AI 300-series processor
- ✓ Windows 10/11 (64-bit)
- ✓ Visual Studio 2022 (or Build Tools)
- ✓ CMake 3.20+
- ✓ Ryzen AI Software 1.6.0 installed at `C:\Program Files\RyzenAI\1.6.0`

## Step 2: Build the Server

```cmd
cd src\ryzenai-serve
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

The executable will be at: `build\bin\Release\ryzenai-serve.exe`

## Step 3: Get a Model

You need an ONNX format model compatible with Ryzen AI. Examples:

- **Phi-3 Mini (NPU)**: https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-onnx
- **Qwen2 1.5B (NPU)**: https://huggingface.co/amd/Qwen2-1.5B-onnx-ryzenai-npu
- **Llama-3.2 (Hybrid)**: Follow Ryzen AI documentation for conversion

Models are typically cached in:
```
C:\Users\<YourName>\.cache\huggingface\hub\
```

## Step 4: Start the Server

```cmd
cd build\bin\Release

# Basic usage (auto-detect mode)
ryzenai-serve.exe -m C:\path\to\your\onnx\model

# Specify NPU mode
ryzenai-serve.exe -m C:\path\to\your\onnx\model --mode npu

# Hybrid mode on custom port
ryzenai-serve.exe -m C:\path\to\your\onnx\model --mode hybrid --port 8081
```

You should see:
```
╔═══════════════════════════════════════════════════════════════╗
║            Ryzen AI LLM Server                                 ║
║            OpenAI API Compatible                               ║
╚═══════════════════════════════════════════════════════════════╝

[Server] Loading model...
[InferenceEngine] Model loaded successfully: phi-3-mini-4k-instruct
[Server] ✓ Model loaded: phi-3-mini-4k-instruct
[Server] ✓ Execution mode: npu
[Server] ✓ Max prompt length: 4096 tokens

═══════════════════════════════════════════════════════════════
  Server running at: http://localhost:8080
═══════════════════════════════════════════════════════════════
```

## Step 5: Test the Server

### Health Check

```cmd
curl http://localhost:8080/health
```

Expected response:
```json
{
  "status": "ok",
  "model": "phi-3-mini-4k-instruct",
  "execution_mode": "npu",
  "max_prompt_length": 4096,
  "ryzenai_version": "1.6.0"
}
```

### Text Completion

```cmd
curl http://localhost:8080/v1/completions ^
  -H "Content-Type: application/json" ^
  -d "{\"prompt\": \"The quick brown fox\", \"max_tokens\": 50}"
```

### Chat Completion

```cmd
curl http://localhost:8080/v1/chat/completions ^
  -H "Content-Type: application/json" ^
  -d "{\"messages\": [{\"role\": \"user\", \"content\": \"Hello! How are you?\"}], \"max_tokens\": 100}"
```

### Streaming Chat

```cmd
curl http://localhost:8080/v1/chat/completions ^
  -H "Content-Type: application/json" ^
  -d "{\"messages\": [{\"role\": \"user\", \"content\": \"Tell me a story\"}], \"max_tokens\": 200, \"stream\": true}"
```

## Step 6: Integrate with Applications

The server is OpenAI API compatible, so you can use it with:

### Python (OpenAI SDK)

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://localhost:8080/v1",
    api_key="not-needed"  # No auth required
)

response = client.chat.completions.create(
    model="ignored",  # Model already loaded
    messages=[
        {"role": "user", "content": "Hello!"}
    ]
)

print(response.choices[0].message.content)
```

### Continue.dev / Cursor

In your Continue/Cursor config:
```json
{
  "models": [
    {
      "title": "Ryzen AI Local",
      "provider": "openai",
      "model": "local-model",
      "apiBase": "http://localhost:8080/v1",
      "apiKey": "not-needed"
    }
  ]
}
```

### Open WebUI

Add a new connection:
- **API Base URL**: `http://localhost:8080/v1`
- **API Key**: (leave blank or use any string)

## Common Issues

### "Model not found" error

Ensure the model path is correct and contains:
- `genai_config.json`
- `model.onnx`
- Tokenizer files

### "Failed to load model" error

Check:
1. Ryzen AI 1.6.0 is installed correctly
2. NPU drivers are up to date (version 32.0.130.1018+)
3. Model is compatible with your Ryzen AI version

### Server won't start

Check if port 8080 is already in use. Try a different port:
```cmd
ryzenai-serve.exe -m C:\path\to\model --port 8081
```

## Next Steps

- Read [README.md](README.md) for detailed architecture
- See [BUILD.md](BUILD.md) for build troubleshooting
- Check Ryzen AI docs: https://ryzenai.docs.amd.com

## Multiple Models

To serve multiple models simultaneously, start multiple server instances:

```cmd
# Terminal 1: Phi-3 on port 8080
ryzenai-serve.exe -m C:\models\phi-3-onnx --port 8080

# Terminal 2: Qwen on port 8081
ryzenai-serve.exe -m C:\models\qwen-onnx --port 8081

# Terminal 3: Llama on port 8082
ryzenai-serve.exe -m C:\models\llama-onnx --port 8082
```

Now you have three models available at different endpoints!

## Support

For issues and questions:
- Ryzen AI Documentation: https://ryzenai.docs.amd.com
- ONNX Runtime GenAI: https://github.com/microsoft/onnxruntime-genai
- AMD Developer Forums: https://community.amd.com

Happy inferencing! 🚀

