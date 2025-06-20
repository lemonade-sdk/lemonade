import socketserver
import sys
import logging
import importlib
import asyncio
from contextlib import asynccontextmanager
from fastapi import FastAPI

_lazy_imports = {
    "TextIteratorStreamer": ("transformers", "TextIteratorStreamer"),
    "StoppingCriteriaList": ("transformers", "StoppingCriteriaList"),
}


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
    # Only do minimal setup here so endpoints are available immediately
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

    # Start lazy imports in the background, and set app.initialized = True
    # when the imports are available
    async def lazy_imports_bg():
        for object_name, import_info in _lazy_imports.items():
            module_name = import_info[0]
            class_name = import_info[1]
            module = importlib.import_module(module_name)
            obj = getattr(module, class_name)
            globals()[object_name] = obj

        app.initialized = True

    asyncio.create_task(lazy_imports_bg())

    yield
