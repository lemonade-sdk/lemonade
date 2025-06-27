import json
import os
from typing import Optional
import shutil
import huggingface_hub
from importlib.metadata import distributions
from lemonade_server.pydantic_models import PullConfig
from lemonade.cache import DEFAULT_CACHE_DIR

USER_MODELS_FILE = os.path.join(DEFAULT_CACHE_DIR, "user_models.json")


class ModelManager:

    @staticmethod
    def parse_checkpoint(checkpoint: str) -> tuple[str, str | None]:
        """
        Parse a checkpoint string that may contain a variant separated by a colon.

        For GGUF models, the format is "repository:variant" (e.g., "unsloth/Qwen3-0.6B-GGUF:Q4_0").
        For other models, there is no variant.

        Args:
            checkpoint: The checkpoint string, potentially with variant

        Returns:
            tuple: (base_checkpoint, variant) where variant is None if no colon is present
        """
        if ":" in checkpoint:
            base_checkpoint, variant = checkpoint.split(":", 1)
            return base_checkpoint, variant
        return checkpoint, None

    @property
    def supported_models(self) -> dict:
        """
        Returns a dictionary of supported models.
        Note: Models must be downloaded before they are locally available.
        """
        # Load the models dictionary from the built-in JSON file
        server_models_file = os.path.join(
            os.path.dirname(__file__), "server_models.json"
        )
        with open(server_models_file, "r", encoding="utf-8") as file:
            models: dict = json.load(file)

        # Load the user's JSON file, if it exists, and merge into the models dict
        if os.path.exists(USER_MODELS_FILE):
            with open(USER_MODELS_FILE, "r", encoding="utf-8") as file:
                user_models: dict = json.load(file)
            # Prepend the user namespace to the model names
            user_models = {
                f"user.{model_name}": model_info
                for model_name, model_info in user_models.items()
            }

            models.update(user_models)

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
        downloaded_checkpoints = self.downloaded_hf_checkpoints
        for model in self.supported_models:
            base_checkpoint = self.parse_checkpoint(
                self.supported_models[model]["checkpoint"]
            )[0]
            if base_checkpoint in downloaded_checkpoints:
                downloaded_models[model] = self.supported_models[model]
        return downloaded_models

    @property
    def downloaded_models_enabled(self) -> dict:
        """
        Returns a dictionary of locally available models that are enabled by
        the current installation.
        """
        return self.filter_models_by_backend(self.downloaded_models)

    def identify_gguf_models(
        self, checkpoint: str, variant: str, mmproj: str
    ) -> tuple[dict, list[str]]:
        """
        Identifies the GGUF model files in the repository that match the variant.
        """

        hint = """
        The CHECKPOINT:VARIANT scheme is used to specify model files in Hugging Face repositories.

        The VARIANT format can be one of several types:
        1. Full filename: exact file to download
        2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
        3. Quantization variant: find a single file ending with the variant name (case insensitive)
        4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)
        
        Examples:
        - "unsloth/Qwen3-8B-GGUF:qwen3.gguf" -> downloads "qwen3.gguf"
        - "unsloth/Qwen3-30B-A3B-GGUF" -> downloads "Qwen3-30B-A3B-GGUF.gguf"
        - "unsloth/Qwen3-8B-GGUF:Q4_1" -> downloads "Qwen3-8B-GGUF-Q4_1.gguf"
        - "unsloth/Qwen3-30B-A3B-GGUF:Q4_0" -> downloads all files in "Q4_0/" folder
        """

        repo_files = huggingface_hub.list_repo_files(checkpoint)
        sharded_files = []

        # (case 1) If variant ends in .gguf, use it directly
        if variant and variant.endswith(".gguf"):
            variant_name = variant
            if variant_name not in repo_files:
                raise ValueError(
                    f"File {variant} not found in Hugging Face repository {checkpoint}. {hint}"
                )
        # (case 2) If no variant is provided, get the first .gguf file in the repository
        elif variant is None:
            all_variants = [
                f for f in repo_files if f.endswith(".gguf") and "mmproj" not in f
            ]
            if len(all_variants) == 0:
                raise ValueError(
                    f"No .gguf files found in Hugging Face repository {checkpoint}. {hint}"
                )
            variant_name = all_variants[0]
        else:
            # (case 3) Find a single file ending with the variant name (case insensitive)
            end_with_variant = [
                f
                for f in repo_files
                if f.lower().endswith(f"{variant}.gguf".lower())
                and "mmproj" not in f.lower()
            ]
            if len(end_with_variant) == 1:
                variant_name = end_with_variant[0]
            elif len(end_with_variant) > 1:
                raise ValueError(
                    f"Multiple .gguf files found for variant {variant}, but only one is allowed. {hint}"
                )
            # (case 4) Check whether the variant corresponds to a folder with sharded files (case insensitive)
            else:
                sharded_files = [
                    f
                    for f in repo_files
                    if f.endswith(".gguf")
                    and f.lower().startswith(f"{variant}/".lower())
                ]

                if not sharded_files:
                    raise ValueError(
                        f"No .gguf files found for variant {variant}. {hint}"
                    )

                # Sort to ensure consistent ordering
                sharded_files.sort()

                # Use first file as primary (this is how llamacpp handles it)
                variant_name = sharded_files[0]

        core_files = {"variant": variant_name}

        # If there is a mmproj file, add it to the patterns
        if mmproj:
            if mmproj not in repo_files:
                raise ValueError(
                    f"The provided mmproj file {mmproj} was not found in {checkpoint}."
                )
            core_files["mmproj"] = mmproj

        return core_files, sharded_files

    def download_gguf(self, model_config: PullConfig) -> dict:
        """
        Downloads the GGUF file for the given model configuration.

        For sharded models, if the variant points to a folder (e.g. Q4_0), all files in that folder
        will be downloaded but only the first file will be returned for loading.
        """

        # This code handles all cases by constructing the appropriate filename or pattern
        checkpoint, variant = self.parse_checkpoint(model_config.checkpoint)

        # Identify the GGUF model files in the repository that match the variant
        core_files, sharded_files = self.identify_gguf_models(
            checkpoint, variant, model_config.mmproj
        )

        # Download the files
        snapshot_folder = huggingface_hub.snapshot_download(
            repo_id=checkpoint,
            allow_patterns=list(core_files.values()) + sharded_files,
        )

        # Ensure we downloaded all expected files
        for file in list(core_files.values()) + sharded_files:
            expected_path = os.path.join(snapshot_folder, file)
            if not os.path.exists(expected_path):
                raise ValueError(
                    f"Hugging Face snapshot download for {model_config.checkpoint} "
                    f"expected file {file} not found at {expected_path}"
                )

        # Return a dict of the full path of the core GGUF files
        return {
            file_name: os.path.join(snapshot_folder, file_path)
            for file_name, file_path in core_files.items()
        }

    def download_models(
        self,
        models: list[str],
        checkpoint: Optional[str] = None,
        recipe: Optional[str] = None,
        reasoning: bool = False,
        mmproj: str = "",
    ):
        """
        Downloads the specified models from Hugging Face.
        """
        for model in models:
            if model not in self.supported_models:
                # Register the model as a user model if the model name
                # is not already registered

                # Ensure the model name includes the `user` namespace
                model_parsed = model.split(".", 1)
                if len(model_parsed) != 2 or model_parsed[0] != "user":
                    raise ValueError(
                        f"When registering a new model, the model name must "
                        "include the `user` namespace, for example "
                        f"`user.Phi-4-Mini-GGUF`. Received: {model}"
                    )

                model_name = model_parsed[1]

                # Check that required arguments are provided
                if not recipe or not checkpoint:
                    raise ValueError(
                        f"Model {model} is not registered with Lemonade Server. "
                        "To register and install it, provide the `checkpoint` "
                        "and `recipe` arguments, as well as the optional "
                        "`reasoning` and `mmproj` arguments as appropriate. "
                    )

                # JSON content that will be used for registration if the download succeeds
                new_user_model = {
                    "checkpoint": checkpoint,
                    "recipe": recipe,
                    "reasoning": reasoning,
                    "suggested": True,
                    "labels": ["custom"],
                }

                if mmproj:
                    new_user_model["mmproj"] = mmproj

                # Make sure that a variant is provided for GGUF models before registering the model
                if "gguf" in checkpoint.lower() and ":" not in checkpoint.lower():
                    raise ValueError(
                        "You are required to provide a 'variant' in the checkpoint field when "
                        "registering a GGUF model. The variant is provided "
                        "as CHECKPOINT:VARIANT. For example: "
                        "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
                        "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:"
                        "qwen2.5-coder-3b-instruct-q4_0.gguf"
                    )

                # Create a PullConfig we will use to download the model
                new_registration_model_config = PullConfig(
                    model_name=model_name,
                    checkpoint=checkpoint,
                    recipe=recipe,
                    reasoning=reasoning,
                )
            else:
                new_registration_model_config = None

            # Download the model
            if new_registration_model_config:
                checkpoint_to_download = checkpoint
                gguf_model_config = new_registration_model_config
            else:
                checkpoint_to_download = self.supported_models[model]["checkpoint"]
                gguf_model_config = PullConfig(**self.supported_models[model])
            print(f"Downloading {model} ({checkpoint_to_download})")

            if "gguf" in checkpoint_to_download.lower():
                self.download_gguf(gguf_model_config)
            else:
                huggingface_hub.snapshot_download(repo_id=checkpoint_to_download)

            # Register the model in user_models.json, creating that file if needed
            # We do this registration after the download so that we don't register
            # any incorrectly configured models where the download would fail
            if new_registration_model_config:
                if os.path.exists(USER_MODELS_FILE):
                    with open(USER_MODELS_FILE, "r", encoding="utf-8") as file:
                        user_models: dict = json.load(file)
                else:
                    user_models = {}

                user_models[model_name] = new_user_model

                # Ensure the cache directory exists before writing the file
                os.makedirs(os.path.dirname(USER_MODELS_FILE), exist_ok=True)

                with open(USER_MODELS_FILE, mode="w", encoding="utf-8") as file:
                    json.dump(user_models, fp=file)

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

    def delete_model(self, model_name: str):
        """
        Deletes the specified model from local storage.
        """
        if model_name not in self.supported_models:
            raise ValueError(
                f"Model {model_name} is not supported. Please choose from the following: "
                f"{list(self.supported_models.keys())}"
            )

        checkpoint = self.supported_models[model_name]["checkpoint"]
        print(f"Deleting {model_name} ({checkpoint})")

        # Handle GGUF models that have the format "checkpoint:variant"
        base_checkpoint = self.parse_checkpoint(checkpoint)[0]

        try:
            # Get the local path using snapshot_download with local_files_only=True
            snapshot_path = huggingface_hub.snapshot_download(
                repo_id=base_checkpoint, local_files_only=True
            )

            # Navigate up to the model directory (parent of snapshots directory)
            model_path = os.path.dirname(os.path.dirname(snapshot_path))

            # Delete the entire model directory (including all snapshots)
            if os.path.exists(model_path):
                shutil.rmtree(model_path)
                print(f"Successfully deleted model {model_name} from {model_path}")
            else:
                raise ValueError(
                    f"Model {model_name} not found locally at {model_path}"
                )

        except Exception as e:
            if (
                "not found in cache" in str(e).lower()
                or "no such file" in str(e).lower()
            ):
                raise ValueError(f"Model {model_name} is not installed locally")
            else:
                raise ValueError(f"Failed to delete model {model_name}: {str(e)}")


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
