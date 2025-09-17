import json
import os
from typing import Optional
import shutil
import huggingface_hub
from importlib.metadata import distributions
from lemonade_server.pydantic_models import PullConfig
from lemonade_server.pydantic_models import PullConfig
from lemonade.cache import DEFAULT_CACHE_DIR
from lemonade.tools.llamacpp.utils import parse_checkpoint, download_gguf
from lemonade.common.network import custom_snapshot_download

USER_MODELS_FILE = os.path.join(DEFAULT_CACHE_DIR, "user_models.json")


class ModelManager:

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

            # Backwards compatibility for user models that were created before version 8.0.4
            # "reasoning" was a boolean, but as of 8.0.4 it became a label
            for _, model_info in user_models.items():
                if "reasoning" in model_info:
                    model_info["labels"] = (
                        ["reasoning"]
                        if not model_info.get("labels", None)
                        else model_info["labels"] + ["reasoning"]
                    )
                    del model_info["reasoning"]

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
        For GGUF models with variants, checks if the specific variant files exist.
        """
        downloaded_models = {}
        downloaded_checkpoints = self.downloaded_hf_checkpoints
        for model in self.supported_models:
            model_info = self.supported_models[model]
            checkpoint = model_info["checkpoint"]
            base_checkpoint, variant = parse_checkpoint(checkpoint)

            if base_checkpoint in downloaded_checkpoints:
                # For GGUF models with variants, verify the specific variant files exist
                if variant and model_info.get("recipe") == "llamacpp":
                    try:
                        from lemonade.tools.llamacpp.utils import identify_gguf_models
                        from lemonade.common.network import custom_snapshot_download

                        # Get the local snapshot path
                        snapshot_path = custom_snapshot_download(
                            base_checkpoint, local_files_only=True
                        )

                        # Check if the specific variant files exist
                        core_files, sharded_files = identify_gguf_models(
                            base_checkpoint, variant, model_info.get("mmproj", "")
                        )
                        all_variant_files = list(core_files.values()) + sharded_files

                        # Verify all required files exist locally
                        all_files_exist = True
                        for file_path in all_variant_files:
                            full_file_path = os.path.join(snapshot_path, file_path)
                            if not os.path.exists(full_file_path):
                                all_files_exist = False
                                break

                        if all_files_exist:
                            downloaded_models[model] = model_info

                    except Exception:
                        # If we can't verify the variant, don't include it
                        pass
                else:
                    # For non-GGUF models or GGUF without variants, use the original logic
                    downloaded_models[model] = model_info
        return downloaded_models

    @property
    def downloaded_models_enabled(self) -> dict:
        """
        Returns a dictionary of locally available models that are enabled by
        the current installation.
        """
        return self.filter_models_by_backend(self.downloaded_models)

    def download_models(
        self,
        models: list[str],
        checkpoint: Optional[str] = None,
        recipe: Optional[str] = None,
        reasoning: bool = False,
        vision: bool = False,
        mmproj: str = "",
        do_not_upgrade: bool = False,
    ):
        """
        Downloads the specified models from Hugging Face.

        do_not_upgrade: prioritize any local copy of the model over any updated copy
            from the Hugging Face Hub.
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
                labels = ["custom"]
                if reasoning:
                    labels.append("reasoning")
                if vision:
                    labels.append("vision")

                new_user_model = {
                    "checkpoint": checkpoint,
                    "recipe": recipe,
                    "suggested": True,
                    "labels": labels,
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
                    vision=vision,
                )
            else:
                # Model is already registered - check if trying to register with different parameters
                existing_model = self.supported_models[model]
                existing_checkpoint = existing_model.get("checkpoint")
                existing_recipe = existing_model.get("recipe")
                existing_reasoning = "reasoning" in existing_model.get("labels", [])
                existing_mmproj = existing_model.get("mmproj", "")
                existing_vision = "vision" in existing_model.get("labels", [])

                # Compare parameters
                checkpoint_differs = checkpoint and checkpoint != existing_checkpoint
                recipe_differs = recipe and recipe != existing_recipe
                reasoning_differs = reasoning and reasoning != existing_reasoning
                mmproj_differs = mmproj and mmproj != existing_mmproj
                vision_differs = vision and vision != existing_vision

                if (
                    checkpoint_differs
                    or recipe_differs
                    or reasoning_differs
                    or mmproj_differs
                    or vision_differs
                ):
                    conflicts = []
                    if checkpoint_differs:
                        conflicts.append(
                            f"checkpoint (existing: '{existing_checkpoint}', new: '{checkpoint}')"
                        )
                    if recipe_differs:
                        conflicts.append(
                            f"recipe (existing: '{existing_recipe}', new: '{recipe}')"
                        )
                    if reasoning_differs:
                        conflicts.append(
                            f"reasoning (existing: {existing_reasoning}, new: {reasoning})"
                        )
                    if mmproj_differs:
                        conflicts.append(
                            f"mmproj (existing: '{existing_mmproj}', new: '{mmproj}')"
                        )
                    if vision_differs:
                        conflicts.append(
                            f"vision (existing: {existing_vision}, new: {vision})"
                        )

                    conflict_details = ", ".join(conflicts)

                    additional_suggestion = ""
                    if model.startswith("user."):
                        additional_suggestion = f" or delete the existing model first using `lemonade-server delete {model}`"

                    raise ValueError(
                        f"Model {model} is already registered with a different configuration. "
                        f"Conflicting parameters: {conflict_details}. "
                        f"Please use a different model name{additional_suggestion}."
                    )
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
                download_gguf(
                    gguf_model_config.checkpoint,
                    gguf_model_config.mmproj,
                    do_not_upgrade=do_not_upgrade,
                )
            else:
                custom_snapshot_download(
                    checkpoint_to_download, do_not_upgrade=do_not_upgrade
                )

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
        current environment and platform.
        """
        import platform

        installed_packages = {dist.metadata["Name"].lower() for dist in distributions()}

        hybrid_installed = (
            "onnxruntime-vitisai" in installed_packages
            and "onnxruntime-genai-directml-ryzenai" in installed_packages
        )

        # On macOS, only llamacpp (GGUF) models are supported, and only on Apple Silicon with macOS 14+
        is_macos = platform.system() == "Darwin"
        if is_macos:
            machine = platform.machine().lower()
            if machine == "x86_64":
                # Intel Macs are not supported - return empty model list with error info
                return {
                    "_unsupported_platform_error": {
                        "error": "Intel Mac Not Supported",
                        "message": (
                            "Lemonade Server requires Apple Silicon processors on macOS. "
                            "Intel Macs are not currently supported. "
                            "Please use a Mac with Apple Silicon or try Lemonade on Windows/Linux."
                        ),
                        "platform": f"macOS {machine}",
                        "supported": "macOS 14+ with Apple Silicon (arm64/aarch64)",
                    }
                }

            # Check macOS version requirement
            mac_version = platform.mac_ver()[0]
            if mac_version:
                major_version = int(mac_version.split(".")[0])
                if major_version < 14:
                    return {
                        "_unsupported_platform_error": {
                            "error": "macOS Version Not Supported",
                            "message": (
                                f"Lemonade Server requires macOS 14 or later. "
                                f"Your system is running macOS {mac_version}. "
                                f"Please update your macOS version to use Lemonade Server."
                            ),
                            "platform": f"macOS {mac_version} {machine}",
                            "supported": "macOS 14+ with Apple Silicon (arm64/aarch64)",
                        }
                    }

        filtered = {}
        for model, value in models.items():
            recipe = value.get("recipe")

            # Filter OGA hybrid models based on package availability
            if recipe == "oga-hybrid":
                if not hybrid_installed:
                    continue

            # On macOS, only show llamacpp models (GGUF format)
            if is_macos and recipe != "llamacpp":
                continue

            filtered[model] = value

        return filtered

    def delete_model(self, model_name: str):
        """
        Deletes the specified model from local storage.
        For GGUF models with variants, only deletes the specific variant files.
        """
        if model_name not in self.supported_models:
            raise ValueError(
                f"Model {model_name} is not supported. Please choose from the following: "
                f"{list(self.supported_models.keys())}"
            )

        checkpoint = self.supported_models[model_name]["checkpoint"]
        print(f"Deleting {model_name} ({checkpoint})")

        # Parse checkpoint to get base and variant
        base_checkpoint, variant = parse_checkpoint(checkpoint)

        # Get the repository cache directory
        snapshot_path = None
        model_cache_dir = None
        try:
            # First, try to get the local path using snapshot_download with local_files_only=True
            snapshot_path = custom_snapshot_download(
                base_checkpoint, local_files_only=True
            )
            # Navigate up to the model directory (parent of snapshots directory)
            model_cache_dir = os.path.dirname(os.path.dirname(snapshot_path))

        except Exception as e:
            # If snapshot_download fails, try to construct the cache path manually
            if (
                "not found in cache" in str(e).lower()
                or "localentrynotfounderror" in str(e).lower()
                or "cannot find an appropriate cached snapshot" in str(e).lower()
            ):
                # Construct the Hugging Face cache path manually
                cache_home = huggingface_hub.constants.HF_HUB_CACHE
                # Convert repo format (e.g., "unsloth/GLM-4.5-Air-GGUF") to cache format
                repo_cache_name = base_checkpoint.replace("/", "--")
                model_cache_dir = os.path.join(cache_home, f"models--{repo_cache_name}")
                # Try to find the snapshot path within the model cache directory
                if os.path.exists(model_cache_dir):
                    snapshots_dir = os.path.join(model_cache_dir, "snapshots")
                    if os.path.exists(snapshots_dir):
                        snapshot_dirs = [
                            d
                            for d in os.listdir(snapshots_dir)
                            if os.path.isdir(os.path.join(snapshots_dir, d))
                        ]
                        if snapshot_dirs:
                            # Use the first (likely only) snapshot directory
                            snapshot_path = os.path.join(
                                snapshots_dir, snapshot_dirs[0]
                            )
            else:
                raise ValueError(f"Failed to delete model {model_name}: {str(e)}")

        # Handle deletion based on whether this is a GGUF model with variants
        if variant and snapshot_path and os.path.exists(snapshot_path):
            # This is a GGUF model with a specific variant - delete only variant files
            try:
                from lemonade.tools.llamacpp.utils import identify_gguf_models

                # Get the specific files for this variant
                core_files, sharded_files = identify_gguf_models(
                    base_checkpoint,
                    variant,
                    self.supported_models[model_name].get("mmproj", ""),
                )
                all_variant_files = list(core_files.values()) + sharded_files

                # Delete the specific variant files
                deleted_files = []
                for file_path in all_variant_files:
                    full_file_path = os.path.join(snapshot_path, file_path)
                    if os.path.exists(full_file_path):
                        if os.path.isfile(full_file_path):
                            os.remove(full_file_path)
                            deleted_files.append(file_path)
                        elif os.path.isdir(full_file_path):
                            shutil.rmtree(full_file_path)
                            deleted_files.append(file_path)

                if deleted_files:
                    print(f"Successfully deleted variant files: {deleted_files}")
                else:
                    print(f"No variant files found for {variant} in {snapshot_path}")

                # Check if the snapshot directory is now empty (only containing .gitattributes, README, etc.)
                remaining_files = [
                    f
                    for f in os.listdir(snapshot_path)
                    if f.endswith(".gguf")
                    or os.path.isdir(os.path.join(snapshot_path, f))
                ]

                # If no GGUF files remain, we can delete the entire repository
                if not remaining_files:
                    print(f"No other variants remain, deleting entire repository cache")
                    shutil.rmtree(model_cache_dir)
                    print(
                        f"Successfully deleted entire model cache at {model_cache_dir}"
                    )
                else:
                    print(
                        f"Other variants still exist in repository, keeping cache directory"
                    )

            except Exception as variant_error:
                print(
                    f"Warning: Could not perform selective variant deletion: {variant_error}"
                )
                print("This may indicate the files were already manually deleted")

        elif model_cache_dir and os.path.exists(model_cache_dir):
            # Non-GGUF model or GGUF without variant - delete entire repository as before
            shutil.rmtree(model_cache_dir)
            print(f"Successfully deleted model {model_name} from {model_cache_dir}")

        elif model_cache_dir:
            # Model directory doesn't exist - it was likely already manually deleted
            print(
                f"Model {model_name} directory not found at {model_cache_dir} - may have been manually deleted"
            )

        else:
            raise ValueError(f"Unable to determine cache path for model {model_name}")

        # Clean up user models registry if applicable
        if model_name.startswith("user.") and os.path.exists(USER_MODELS_FILE):
            with open(USER_MODELS_FILE, "r", encoding="utf-8") as file:
                user_models = json.load(file)

            # Remove the "user." prefix to get the actual model name in the file
            base_model_name = model_name[5:]  # Remove "user." prefix

            if base_model_name in user_models:
                del user_models[base_model_name]
                with open(USER_MODELS_FILE, "w", encoding="utf-8") as file:
                    json.dump(user_models, file)
                print(f"Removed {model_name} from user models registry")


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
