"""Lightweight HuggingFace cache scanner without huggingface_hub dependency."""

import os
from pathlib import Path
from typing import List, Optional


def get_hf_cache_dir() -> Path:
    """
    Get the HuggingFace cache directory.

    Checks in order:
    1. HF_HUB_CACHE environment variable
    2. HF_HOME environment variable + /hub
    3. Default location based on OS

    Returns:
        Path to the HuggingFace cache directory
    """
    # Check HF_HUB_CACHE first (most specific)
    if "HF_HUB_CACHE" in os.environ:
        return Path(os.environ["HF_HUB_CACHE"])

    # Check HF_HOME (should append /hub)
    if "HF_HOME" in os.environ:
        return Path(os.environ["HF_HOME"]) / "hub"

    # Default location
    home = Path.home()
    return home / ".cache" / "huggingface" / "hub"


def scan_hf_cache() -> List[str]:
    """
    Scan the HuggingFace cache directory for downloaded models.

    Returns a list of model repo IDs (e.g., ["microsoft/phi-2", "meta-llama/Llama-2-7b"]).
    This is a lightweight implementation that doesn't require huggingface_hub.

    Returns:
        List of repository IDs found in the cache
    """
    cache_dir = get_hf_cache_dir()

    if not cache_dir.exists():
        return []

    repo_ids = []

    try:
        # List all directories in the cache
        for item in cache_dir.iterdir():
            if not item.is_dir():
                continue

            # HuggingFace cache uses the format: models--{org}--{model}
            # Example: models--microsoft--phi-2 -> microsoft/phi-2
            name = item.name

            if name.startswith("models--"):
                # Remove the "models--" prefix
                repo_name = name[8:]  # len("models--") = 8

                # Replace first "--" with "/" to get org/model format
                # Handle multiple dashes by only replacing the first occurrence
                parts = repo_name.split("--", 1)
                if len(parts) == 2:
                    repo_id = f"{parts[0]}/{parts[1]}"
                    repo_ids.append(repo_id)

    except Exception as e:
        print(f"Error scanning HuggingFace cache: {e}")

    return sorted(repo_ids)


def get_model_snapshot_path(repo_id: str) -> Optional[Path]:
    """
    Get the path to a model's snapshot in the HuggingFace cache.

    Args:
        repo_id: Repository ID in the format "org/model" (e.g., "microsoft/phi-2")

    Returns:
        Path to the model snapshot, or None if not found
    """
    cache_dir = get_hf_cache_dir()

    # Convert repo_id to cache format: org/model -> models--org--model
    cache_name = f"models--{repo_id.replace('/', '--')}"
    model_dir = cache_dir / cache_name

    if not model_dir.exists():
        return None

    # Find the snapshot directory (usually there's only one)
    snapshots_dir = model_dir / "snapshots"
    if not snapshots_dir.exists():
        return None

    try:
        # Get the first snapshot directory (there's typically only one)
        snapshot_dirs = [d for d in snapshots_dir.iterdir() if d.is_dir()]
        if snapshot_dirs:
            # Return the most recently modified snapshot
            return max(snapshot_dirs, key=lambda d: d.stat().st_mtime)
    except Exception as e:
        print(f"Error finding snapshot for {repo_id}: {e}")

    return None


def find_gguf_models() -> List[tuple[str, str]]:
    """
    Find all GGUF models in the HuggingFace cache.

    Returns:
        List of tuples (repo_id, gguf_filename) for each GGUF file found
    """
    gguf_models = []
    repo_ids = scan_hf_cache()

    for repo_id in repo_ids:
        snapshot_path = get_model_snapshot_path(repo_id)
        if not snapshot_path:
            continue

        try:
            # Look for .gguf files in the snapshot
            for file_path in snapshot_path.glob("**/*.gguf"):
                if file_path.is_file():
                    gguf_models.append((repo_id, file_path.name))
        except Exception as e:
            print(f"Error scanning {repo_id}: {e}")

    return sorted(gguf_models)
