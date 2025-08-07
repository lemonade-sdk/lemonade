import json
import os

# Define the paths
json_file_path = os.path.join(
    os.path.dirname(__file__), "..", "src", "lemonade_server", "server_models.json"
)
markdown_file_path = os.path.join(
    os.path.dirname(__file__), "server", "server_models.md"
)

# Load the JSON data
with open(json_file_path, "r", encoding="utf-8") as json_file:
    models = json.load(json_file)

# Generate the markdown content
markdown_content = r"""
# 🍋 Lemonade Server Models
 
This document provides the models we recommend for use with Lemonade Server.

Click on any model to learn more details about it, such as the [Lemonade Recipe](https://github.com/lemonade-sdk/lemonade/blob/main/docs/lemonade_api.md) used to load the model. Content:

- [Model Management GUI](#model-management-gui)
- [Supported Models](#supported-models)
- [Naming Convention](#naming-convention)
- [Model Storage and Management](#model-storage-and-management)
- [Installing Additional Models](#installing-additional-models)

## Model Management GUI

Lemonade Server offers a model management GUI to help you see which models are available, install new models, and delete models. You can access this GUI by starting Lemonade Server, opening http://localhost:8000 in your web browser, and clicking the Model Management tab.

## Supported Models
"""

markdown_bottom_content = r"""

## Naming Convention

The format of each Lemonade name is a combination of the name in the base checkpoint and the backend where the model will run. So, if the base checkpoint is `meta-llama/Llama-3.2-1B-Instruct`, and it has been optimized to run on Hybrid, the resulting name is `Llama-3.2-3B-Instruct-Hybrid`.

## Model Storage and Management

Lemonade Server relies on [Hugging Face Hub](https://huggingface.co/docs/hub/en/index) to manage downloading and storing models on your system. By default, Hugging Face Hub downloads models to `C:\Users\YOUR_USERNAME\.cache\huggingface\hub`.

For example, the Lemonade Server `Llama-3.2-3B-Instruct-Hybrid` model will end up at `C:\Users\YOUR_USERNAME\.cache\huggingface\hub\models--amd--Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid`. If you want to uninstall that model, simply delete that folder.

You can change the directory for Hugging Face Hub by [setting the `HF_HOME` or `HF_HUB_CACHE` environment variables](https://huggingface.co/docs/huggingface_hub/en/package_reference/environment_variables).

## Installing Additional Models

Once you've installed Lemonade Server, you can install any model on this list using the `pull` command in the [`lemonade-server` CLI](./lemonade-server-cli.md).

Example:

```bash
lemonade-server pull Qwen2.5-0.5B-Instruct-CPU
```

> Note: `lemonade-server` is a utility that is added to your PATH when you install Lemonade Server with the GUI installer.
> If you are using Lemonade Server from a Python environment, use the `lemonade-server-dev pull` command instead.
"""

# Separate models into Hot, Hybrid, CPU, and GGUF
hot_models = []
hybrid_models = []
npu_models = []
cpu_models = []
gguf_models = []
for model_name, details in models.items():
    if details.get("suggested", False):
        # Check for hot models first (these can appear in multiple sections)
        if "labels" in details and "hot" in details["labels"]:
            hot_models.append((model_name, details))
        
        if model_name.endswith("-Hybrid"):
            hybrid_models.append((model_name, details))
        elif model_name.endswith("-NPU"):
            npu_models.append((model_name, details))
        elif model_name.endswith("-CPU"):
            cpu_models.append((model_name, details))
        elif model_name.endswith("-GGUF"):
            gguf_models.append((model_name, details))


def model_section_md(title, models):
    section = f"\n### {title}\n\n"
    for model_name, details in models:
        section += f"<details>\n<summary>{model_name}</summary>\n\n"
        section += f"```bash\nlemonade-server pull {model_name}\n```\n\n"
        section += "<table>\n<tr><th>Key</th><th>Value</th></tr>\n"
        for key, value in details.items():
            if key == "checkpoint":
                colon_split = value.split(":")
                checkpoint = colon_split[0]
                variant = None
                if len(colon_split) > 1:
                    variant = colon_split[1]

                hyperlink = (
                    f'<a href="https://huggingface.co/{checkpoint}">{checkpoint}</a>'
                )
                section += f"<tr><td>{key.capitalize()}</td><td>{hyperlink}</td></tr>\n"
                if variant:
                    section += f"<tr><td>GGUF Variant</td><td>{variant}</td></tr>\n"
            elif key == "labels":
                # Pretty-print labels as comma-separated values with nice formatting
                labels_str = ", ".join(value) if isinstance(value, list) else str(value)
                section += (
                    f"<tr><td>{key.capitalize()}</td><td>{labels_str}</td></tr>\n"
                )
            elif key not in ["max_prompt_length", "suggested"]:
                section += f"<tr><td>{key.capitalize()}</td><td>{value}</td></tr>\n"
        section += "</table>\n\n</details>\n\n"
    return section


# Add models sections using the helper function - Hot models first!
markdown_content += model_section_md("🔥 Hot Models", hot_models)
markdown_content += model_section_md("GGUF", gguf_models)
markdown_content += model_section_md("Hybrid", hybrid_models)
markdown_content += model_section_md("NPU", npu_models)
markdown_content += model_section_md("CPU", cpu_models)

# Add the FAQ items at the bottom
markdown_content += markdown_bottom_content


# Write the markdown content to the file
with open(markdown_file_path, "w", encoding="utf-8") as markdown_file:
    markdown_file.write(markdown_content)

print(f"Markdown file generated at {markdown_file_path}")

# Add license comment to the end of the markdown file
with open(markdown_file_path, "a", encoding="utf-8") as markdown_file:
    markdown_file.write(
        "\n<!--This file was originally licensed under Apache 2.0. It has been modified.\nModifications Copyright (c) 2025 AMD-->"
    )
