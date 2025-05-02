import json
import os
import huggingface_hub
import pkg_resources


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
                self.supported_models[model]["checkpoint"]
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
        hybrid_installed = (
            "onnxruntime-vitisai" in pkg_resources.working_set.by_key
            and "onnxruntime-genai-directml-ryzenai" in pkg_resources.working_set.by_key
        )

        downloaded_models_enabled = {}
        for model, value in self.downloaded_models.items():
            if value["recipe"] == "oga-hybrid" and hybrid_installed:
                downloaded_models_enabled[model] = value
            else:
                # All other models are CPU models right now
                # This logic will get more sophisticated when we
                # start to support more backends
                downloaded_models_enabled[model] = value

        return downloaded_models_enabled

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
            huggingface_hub.snapshot_download(repo_id=checkpoint)
