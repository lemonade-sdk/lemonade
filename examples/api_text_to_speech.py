"""
This example demonstrates how to use the lemonade server API to generate
speech using Kokoro via the OpenAI Python client.

Prerequisites:
1. Install the OpenAI client: pip install openai openai[voice_helpers]
2. Start the lemonade server: lemonade-server
3. The kokoro-v1 model will be auto-downloaded on first use

Usage:
    python api_text_to_speech.py
"""

import base64
import asyncio
from pathlib import Path


async def generate_with_openai_client():
    """Generate image using the OpenAI Python client."""
    try:
        from openai import AsyncOpenAI
        from openai.helpers import LocalAudioPlayer
    except ImportError:
        print("OpenAI client not installed. Install with: pip install openai")
        return None

    # Point to local lemonade server
    client = AsyncOpenAI(
        base_url="http://localhost:8000/api/v1",
        api_key="not-needed",  # Lemonade doesn't require API key by default
    )

    print("Generating speech with OpenAI client...")
    print("(This may take several seconds)")

    async with client.audio.speech.with_streaming_response.create(
        model="kokoro-v1",
        voice="coral",
        input="Today is a wonderful day to build something people love!",
        stream_format="audio",
    ) as response:
        await LocalAudioPlayer().play(response)


if __name__ == "__main__":
    print("=" * 60)
    print("Lemonade Text to Speech Example")
    print("=" * 60)
    print()
    print("Make sure the lemonade server is running:")
    print("  lemonade-server")
    print()

    # Generate using OpenAI client
    asyncio.run(generate_with_openai_client())

    print()
    print("=" * 60)
    print("Done!")
