import socketserver
import sys
import logging
from contextlib import asynccontextmanager
from fastapi import FastAPI


def find_free_port():
    """
    Scans for an unoccupied TCP port

    Returns the port number as an int on success
    Returns None if no port can be found
    """

    try:
        with socketserver.TCPServer(("localhost", 0), None) as s:
            return s.server_address[1]
    # pylint: disable=broad-exception-caught
    except Exception:
        return None


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Code here will run when the application starts up
    # Check if console can handle Unicode by testing emoji encoding

    try:
        if sys.stdout.encoding:
            "🍋".encode(sys.stdout.encoding)
        use_emojis = True
    except (UnicodeEncodeError, AttributeError):
        use_emojis = False

    if use_emojis:
        logging.info(
            "\n"
            "\n"
            "🍋  Lemonade Server Ready!\n"
            f"🍋    Open http://localhost:{app.port} in your browser for:\n"
            "🍋      💬 chat\n"
            "🍋      💻 model management\n"
            "🍋      📄 docs\n"
        )
    else:
        logging.info(
            "\n"
            "\n"
            "[Lemonade]  Lemonade Server Ready!\n"
            f"[Lemonade]    Open http://localhost:{app.port} in your browser for:\n"
            "[Lemonade]      chat\n"
            "[Lemonade]      model management\n"
            "[Lemonade]      docs\n"
        )

    yield
