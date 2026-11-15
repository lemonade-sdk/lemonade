"""Inline media payloads for tests that send images or audio in chat messages."""

import base64
import os
import struct
import zlib

SPEECH_WAV = os.path.join(os.path.dirname(os.path.dirname(__file__)), "test_speech.wav")
SPEECH_KEYWORD = "working"


def solid_color_png_data_uri(r, g, b, size=256):
    """Data URI for a solid RGB PNG (stdlib only, no Pillow).

    A test can assert a vision model actually saw the image by asking it to
    name the color back.
    """

    def _chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)  # 8-bit RGB
    row = b"\x00" + bytes((r, g, b)) * size  # filter byte + pixels
    png = (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", ihdr)
        + _chunk(b"IDAT", zlib.compress(row * size, 9))
        + _chunk(b"IEND", b"")
    )
    return "data:image/png;base64," + base64.b64encode(png).decode()


def speech_input_audio_part():
    """OpenAI `input_audio` content part carrying test_speech.wav."""
    with open(SPEECH_WAV, "rb") as f:
        data = base64.b64encode(f.read()).decode()
    return {"type": "input_audio", "input_audio": {"data": data, "format": "wav"}}
