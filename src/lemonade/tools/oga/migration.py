"""
Migration utilities for handling RyzenAI version upgrades.

This module provides functionality to detect and clean up incompatible RyzenAI models
when upgrading between major versions (e.g., 1.4/1.5 -> 1.6).
"""

import os
import json
import shutil
import logging
from typing import List, Dict, Optional, Tuple


def get_directory_size(path: str) -> int:
    """
    Calculate the total size of a directory in bytes.

    Args:
        path: Path to the directory

    Returns:
        Total size in bytes
    """
    total_size = 0
    try:
        for dirpath, _, filenames in os.walk(path):
            for filename in filenames:
                filepath = os.path.join(dirpath, filename)
                try:
                    total_size += os.path.getsize(filepath)
                except (OSError, FileNotFoundError):
                    # Skip files that can't be accessed
                    pass
    except (OSError, FileNotFoundError):
        pass
    return total_size


def format_size(size_bytes: int) -> str:
    """
    Format byte size to human-readable string.

    Args:
        size_bytes: Size in bytes

    Returns:
        Formatted string (e.g., "1.5 GB", "450 MB")
    """
    for unit in ["B", "KB", "MB"]:
        if size_bytes < 1024.0:
            return f"{size_bytes:.1f} {unit}"
        size_bytes /= 1024.0
    return f"{size_bytes:.1f} GB"


def check_rai_config_version(model_path: str, required_version: str = "1.6.0") -> bool:
    """
    Check if a model's rai_config.json contains the required version.

    Args:
        model_path: Path to the model directory
        required_version: Version string to check for (default: "1.6.0")

    Returns:
        True if model is compatible (has required version), False otherwise
    """
    rai_config_path = os.path.join(model_path, "rai_config.json")

    # If no rai_config.json exists, it's not a RyzenAI model
    if not os.path.exists(rai_config_path):
        return True

    try:
        with open(rai_config_path, "r", encoding="utf-8") as f:
            config = json.load(f)

        # Check if max_prompt_length exists and has the required version
        if "max_prompt_length" in config:
            max_prompt_length = config["max_prompt_length"]
            if isinstance(max_prompt_length, dict):
                # If it's a dict with version keys, check for required version
                return required_version in max_prompt_length
            # Fallback to True to avoid deleting models if format changes
            return True

        return True

    except (json.JSONDecodeError, OSError) as e:
        logging.warning(f"Could not read rai_config.json from {model_path}: {e}")
        # If we can't read it, assume it's compatible to avoid false positives
        return True


def scan_oga_models_cache(cache_dir: str) -> List[Dict[str, any]]:
    """
    Scan the Lemonade OGA models cache for incompatible models.

    Args:
        cache_dir: Path to the Lemonade cache directory

    Returns:
        List of dicts with model info (path, name, size, compatible)
    """
    oga_models_path = os.path.join(cache_dir, "oga_models")
    incompatible_models = []

    if not os.path.exists(oga_models_path):
        return incompatible_models

    try:
        # Iterate through model directories in oga_models
        for model_name in os.listdir(oga_models_path):
            model_dir = os.path.join(oga_models_path, model_name)

            if not os.path.isdir(model_dir):
                continue

            # Check all subdirectories (e.g., npu-int4, hybrid-int4)
            for subdir in os.listdir(model_dir):
                subdir_path = os.path.join(model_dir, subdir)

                if not os.path.isdir(subdir_path):
                    continue

                # Check if this model version is compatible
                if not check_rai_config_version(subdir_path):
                    size = get_directory_size(subdir_path)
                    incompatible_models.append(
                        {
                            "path": subdir_path,
                            "name": f"{model_name}/{subdir}",
                            "size": size,
                            "size_formatted": format_size(size),
                            "cache_type": "lemonade",
                        }
                    )

    except (OSError, PermissionError) as e:
        logging.warning(f"Error scanning oga_models cache: {e}")

    return incompatible_models


def scan_huggingface_cache(hf_home: Optional[str] = None) -> List[Dict[str, any]]:
    """
    Scan the HuggingFace cache for incompatible RyzenAI models.

    Args:
        hf_home: Path to HuggingFace home directory (default: from env or ~/.cache/huggingface)

    Returns:
        List of dicts with model info (path, name, size, compatible)
    """
    if hf_home is None:
        hf_home = os.environ.get(
            "HF_HOME", os.path.join(os.path.expanduser("~"), ".cache", "huggingface")
        )

    hub_path = os.path.join(hf_home, "hub")
    incompatible_models = []

    if not os.path.exists(hub_path):
        return incompatible_models

    try:
        # Iterate through model directories in HuggingFace cache
        for item in os.listdir(hub_path):
            if not item.startswith("models--"):
                continue

            model_dir = os.path.join(hub_path, item)
            if not os.path.isdir(model_dir):
                continue

            # Look in snapshots subdirectory
            snapshots_dir = os.path.join(model_dir, "snapshots")
            if not os.path.exists(snapshots_dir):
                continue

            # Check each snapshot
            for snapshot_hash in os.listdir(snapshots_dir):
                snapshot_path = os.path.join(snapshots_dir, snapshot_hash)

                if not os.path.isdir(snapshot_path):
                    continue

                # Check if this snapshot has incompatible RyzenAI model
                if not check_rai_config_version(snapshot_path):
                    # Extract readable model name from directory
                    model_name = item.replace("models--", "").replace("--", "/")
                    size = get_directory_size(
                        model_dir
                    )  # Size of entire model directory
                    incompatible_models.append(
                        {
                            "path": model_dir,
                            "name": model_name,
                            "size": size,
                            "size_formatted": format_size(size),
                            "cache_type": "huggingface",
                        }
                    )
                    break

    except (OSError, PermissionError) as e:
        logging.warning(f"Error scanning HuggingFace cache: {e}")

    return incompatible_models


def detect_incompatible_ryzenai_models(
    cache_dir: str, hf_home: Optional[str] = None
) -> Tuple[List[Dict[str, any]], int]:
    """
    Detect all incompatible RyzenAI models in both Lemonade and HuggingFace caches.

    Args:
        cache_dir: Path to the Lemonade cache directory
        hf_home: Path to HuggingFace home directory (optional)

    Returns:
        Tuple of (list of incompatible models, total size in bytes)
    """
    incompatible_models = []

    # Scan Lemonade cache
    oga_models = scan_oga_models_cache(cache_dir)
    incompatible_models.extend(oga_models)

    # Scan HuggingFace cache
    hf_models = scan_huggingface_cache(hf_home)
    incompatible_models.extend(hf_models)

    # Calculate total size
    total_size = sum(model["size"] for model in incompatible_models)

    logging.info(
        f"Found {len(incompatible_models)} incompatible RyzenAI models "
        f"({format_size(total_size)} total)"
    )

    return incompatible_models, total_size


def delete_model_directory(model_path: str) -> bool:
    """
    Safely delete a model directory.

    Args:
        model_path: Path to the model directory to delete

    Returns:
        True if deletion successful, False otherwise
    """
    try:
        if os.path.exists(model_path):
            shutil.rmtree(model_path)
            logging.info(f"Deleted model directory: {model_path}")
            return True
        else:
            logging.warning(f"Model directory not found: {model_path}")
            return False
    except (OSError, PermissionError) as e:
        logging.error(f"Failed to delete model directory {model_path}: {e}")
        return False


def _extract_checkpoint_from_path(path: str) -> Optional[str]:
    """
    Extract the checkpoint name from a model path.

    Args:
        path: Model directory path (either Lemonade cache or HuggingFace cache)

    Returns:
        Checkpoint name (e.g., "amd/Qwen2.5-1.5B-Instruct-awq") or None if not extractable
    """
    # Normalize path separators to handle both Unix and Windows paths
    normalized_path = path.replace("\\", "/")
    parts = normalized_path.split("/")

    # Handle HuggingFace cache paths: models--{org}--{repo}
    if "models--" in normalized_path:
        for part in parts:
            if part.startswith("models--"):
                # Convert models--org--repo to org/repo
                # Replace first two occurrences of -- with /
                checkpoint = part.replace("models--", "", 1).replace("--", "/", 1)
                return checkpoint
        return None

    # Handle Lemonade cache paths: oga_models/{model_name}/{device}-{dtype}
    if "oga_models" in normalized_path:
        try:
            oga_models_idx = parts.index("oga_models")
            if oga_models_idx + 1 < len(parts):
                model_name = parts[oga_models_idx + 1]
                # Convert model_name back to checkpoint (e.g., amd_model -> amd/model)
                # This is a heuristic - we look for the pattern {org}_{model}
                checkpoint = model_name.replace("_", "/", 1)
                return checkpoint
        except (ValueError, IndexError):
            return None

    return None


def _cleanup_user_models_json(deleted_checkpoints: List[str], user_models_file: str):
    """
    Remove entries from user_models.json for models that have been deleted.

    Args:
        deleted_checkpoints: List of checkpoint names that were deleted
        user_models_file: Path to user_models.json
    """
    if not deleted_checkpoints or not os.path.exists(user_models_file):
        return

    try:
        with open(user_models_file, "r", encoding="utf-8") as f:
            user_models = json.load(f)

        # Track which models to remove
        models_to_remove = []
        for model_name, model_info in user_models.items():
            checkpoint = model_info.get("checkpoint", "")
            # Check if this checkpoint matches any deleted checkpoints
            # We do a case-insensitive comparison since paths may have been lowercased
            for deleted_checkpoint in deleted_checkpoints:
                if checkpoint.lower() == deleted_checkpoint.lower():
                    models_to_remove.append(model_name)
                    break

        # Remove the models
        for model_name in models_to_remove:
            del user_models[model_name]
            logging.info(f"Removed {model_name} from user_models.json")

        # Save the updated file only if we removed something
        if models_to_remove:
            with open(user_models_file, "w", encoding="utf-8") as f:
                json.dump(user_models, f, indent=2)
            logging.info(
                f"Updated user_models.json - removed {len(models_to_remove)} entries"
            )

    except (json.JSONDecodeError, OSError) as e:
        logging.warning(f"Could not update user_models.json: {e}")


def delete_incompatible_models(
    model_paths: List[str], user_models_file: Optional[str] = None
) -> Dict[str, any]:
    """
    Delete multiple incompatible model directories and clean up user_models.json.

    Args:
        model_paths: List of paths to delete
        user_models_file: Path to user_models.json (optional, will use default if not provided)

    Returns:
        Dict with deletion results (success_count, failed_count, freed_size, cleaned_user_models)
    """
    success_count = 0
    failed_count = 0
    freed_size = 0
    deleted_checkpoints = []

    for path in model_paths:
        # Calculate size before deletion
        size = get_directory_size(path)

        # Extract checkpoint name before deleting
        checkpoint = _extract_checkpoint_from_path(path)
        if checkpoint:
            deleted_checkpoints.append(checkpoint)

        if delete_model_directory(path):
            success_count += 1
            freed_size += size
        else:
            failed_count += 1

    # Clean up user_models.json if we deleted any models
    cleaned_user_models = False
    if deleted_checkpoints:
        # Use default path if not provided
        if user_models_file is None:
            from lemonade.cache import DEFAULT_CACHE_DIR

            user_models_file = os.path.join(DEFAULT_CACHE_DIR, "user_models.json")

        _cleanup_user_models_json(deleted_checkpoints, user_models_file)
        cleaned_user_models = True

    return {
        "success_count": success_count,
        "failed_count": failed_count,
        "freed_size": freed_size,
        "freed_size_formatted": format_size(freed_size),
        "cleaned_user_models": cleaned_user_models,
    }
