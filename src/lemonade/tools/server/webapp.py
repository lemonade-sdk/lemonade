from pathlib import Path
import json
import platform
from fastapi.responses import HTMLResponse
from lemonade_server.model_manager import ModelManager


def get_webapp_html(port=8000):
    """
    Show Lemonade Web App for LLM chat and model management.
    """
    # Load server models from JSON
    server_models = ModelManager().supported_models

    # Use shared filter function from model_manager.py
    filtered_models = ModelManager().filter_models_by_backend(server_models)

    # Pass filtered server_models and platform info to JS
    server_models_js = (
        f"<script>window.SERVER_MODELS = {json.dumps(filtered_models)};</script>"
    )
    platform_js = f"<script>window.PLATFORM = '{platform.system()}';</script>"

    # Load HTML template
    template_path = Path(__file__).parent / "static" / "webapp.html"
    with open(template_path, "r", encoding="utf-8") as f:
        html_template = f.read()

    # Replace template variables
    html_content = html_template.replace("{{SERVER_PORT}}", str(port))
    html_content = html_content.replace("{{SERVER_MODELS_JS}}", server_models_js)
    html_content = html_content.replace("{{PLATFORM_JS}}", platform_js)

    return HTMLResponse(content=html_content)
