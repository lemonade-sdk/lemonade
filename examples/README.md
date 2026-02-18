# Lemonade Examples

Interactive demos that work with Lemonade Server.

## Available Demos

### Audio Transcription
- **realtime_transcription.py**: Stream microphone audio for real-time transcription (+ test mode for WAV files)

### LLM Demos
- **llm-debate.html**: A debate arena where multiple LLMs can debate each other on any topic
- **multi-model-tester.html**: Test prompts across multiple models side-by-side

### Image Generation
- **api_image_generation.py**: Generate images using the `images/generations` endpoint

### Speech Synthesis (TTS)
- **api_text_to_speech.py**: Generate speech from a prompt using `audio/speech` endpoint

## Setup

1. Install Lemonade Server from the [latest release](https://github.com/lemonade-sdk/lemonade/releases/latest)
2. Start the server: `lemonade-server serve`
3. Pull a model if needed (e.g., `lemonade-server-dev pull Whisper-Tiny`)

## Running the Examples

### Realtime Transcription (Python)

Uses the OpenAI-compatible WebSocket API for real-time speech-to-text.

```bash
# Install dependencies
pip install openai websockets pyaudio

# Stream from microphone
python realtime_transcription.py

# Use a different model
python realtime_transcription.py --model Whisper-Small
```

### LLM Demos

Open the HTML files directly in your browser.

See [debate-arena.md](debate-arena.md) for detailed instructions on the debate demo.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
