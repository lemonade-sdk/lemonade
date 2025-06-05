import json
import os
import huggingface_hub
from importlib.metadata import distributions
from lemonade_server.pydantic_models import LoadConfig


class ModelManager:

    @property
    def supported_models(self) -> dict:
        """
        Returns a dictionary of supported models.
        Note: Models must be downloaded before they are locally available.
        """
        # Load the models dictionary from the JSON file
        server_models_file = os.path.join(
            os.path.dirname(__file__), "server_models.json"
        )
        with open(server_models_file, "r", encoding="utf-8") as file:
            models = json.load(file)

        # Add the model name as a key in each entry, to make it easier
        # to access later

        for key, value in models.items():
            value["model_name"] = key

        return models

    @property
    def downloaded_hf_checkpoints(self) -> list[str]:
        """
        Returns a list of Hugging Face checkpoints that have been downloaded.
        """
        downloaded_hf_checkpoints = []
        try:
            hf_cache_info = huggingface_hub.scan_cache_dir()
            downloaded_hf_checkpoints = [entry.repo_id for entry in hf_cache_info.repos]
        except huggingface_hub.CacheNotFound:
            pass
        except Exception as e:  # pylint: disable=broad-exception-caught
            print(f"Error scanning Hugging Face cache: {e}")
        return downloaded_hf_checkpoints

    @property
    def downloaded_models(self) -> dict:
        """
        Returns a dictionary of locally available models.
        """
        downloaded_models = {}
        for model in self.supported_models:
            if (
                self.supported_models[model]["checkpoint"].split(":")[0]
                in self.downloaded_hf_checkpoints
            ):
                downloaded_models[model] = self.supported_models[model]
        return downloaded_models

    @property
    def downloaded_models_enabled(self) -> dict:
        """
        Returns a dictionary of locally available models that are enabled by
        the current installation.
        """
        return self.filter_models_by_backend(self.downloaded_models)

    def download_gguf(self, model_config: LoadConfig) -> dict:
        """
        Downloads the GGUF file for the given model configuration.
        """

        # The variant parameter can be either:
        # 1. A full GGUF filename (e.g. "model-Q4_0.gguf")
        # 2. A quantization variant (e.g. "Q4_0")
        # This code handles both cases by constructing the appropriate filename
        checkpoint, variant = model_config.checkpoint.split(":")
        hf_base_name = checkpoint.split("/")[-1].replace("-GGUF", "")
        variant_name = (
            variant if variant.endswith(".gguf") else f"{hf_base_name}-{variant}.gguf"
        )

        # If there is a mmproj file, add it to the patterns
        expected_files = {"variant": variant_name}
        if model_config.mmproj:
            expected_files["mmproj"] = model_config.mmproj

        # Download the files
        snapshot_folder = huggingface_hub.snapshot_download(
            repo_id=checkpoint,
            allow_patterns=list(expected_files.values()),
        )

        # Ensure we downloaded all expected files while creating a dict of the downloaded files
        snapshot_files = {}
        for file in expected_files:
            snapshot_files[file] = os.path.join(snapshot_folder, expected_files[file])
            if expected_files[file] not in os.listdir(snapshot_folder):
                raise ValueError(
                    f"Hugging Face snapshot download for {model_config.checkpoint} "
                    f"expected file {expected_files[file]} not found in {snapshot_folder}"
                )

        # Return a dict that points to the snapshot path of the downloaded GGUF files
        return snapshot_files

    def download_models(self, models: list[str]):
        """
        Downloads the specified models from Hugging Face.
        """
        for model in models:
            if model not in self.supported_models:
                raise ValueError(
                    f"Model {model} is not supported. Please choose from the following: "
                    f"{list(self.supported_models.keys())}"
                )
            checkpoint = self.supported_models[model]["checkpoint"]
            print(f"Downloading {model} ({checkpoint})")

            if "gguf" in checkpoint.lower():
                model_config = LoadConfig(**self.supported_models[model])
                self.download_gguf(model_config)
            else:
                huggingface_hub.snapshot_download(repo_id=checkpoint)

    def filter_models_by_backend(self, models: dict) -> dict:
        """
        Returns a filtered dict of models that are enabled by the
        current environment.
        """
        installed_packages = {dist.metadata["Name"].lower() for dist in distributions()}

        hybrid_installed = (
            "onnxruntime-vitisai" in installed_packages
            and "onnxruntime-genai-directml-ryzenai" in installed_packages
        )
        filtered = {}
        for model, value in models.items():
            if value.get("recipe") == "oga-hybrid":
                if hybrid_installed:
                    filtered[model] = value
            else:
                filtered[model] = value
        return filtered


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
