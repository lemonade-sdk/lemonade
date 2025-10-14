"""
Migration utilities for handling RyzenAI version upgrades.

This module provides functionality to detect and clean up incompatible RyzenAI models
when upgrading between major versions (e.g., 1.4/1.5 -> 1.6).
"""

import os
import json
import shutil
import logging
from pathlib import Path
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
        for dirpath, dirnames, filenames in os.walk(path):
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
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size_bytes < 1024.0:
            return f"{size_bytes:.1f} {unit}"
        size_bytes /= 1024.0
    return f"{size_bytes:.1f} PB"


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
        return True  # Not a RyzenAI model, don't flag as incompatible

    try:
        with open(rai_config_path, "r", encoding="utf-8") as f:
            config = json.load(f)

        # Check if max_prompt_length exists and has the required version
        if "max_prompt_length" in config:
            max_prompt_length = config["max_prompt_length"]
            if isinstance(max_prompt_length, dict):
                # If it's a dict with version keys, check for required version
                return required_version in max_prompt_length
            # If it's not a dict, it's likely a new format - consider compatible
            return True

        # No max_prompt_length field - consider compatible (might be newer format)
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
                    size = get_directory_size(model_dir)  # Size of entire model directory
                    incompatible_models.append(
                        {
                            "path": model_dir,  # Delete entire model dir, not just snapshot
                            "name": model_name,
                            "size": size,
                            "size_formatted": format_size(size),
                            "cache_type": "huggingface",
                        }
                    )
                    break  # Only add model once, even if multiple snapshots

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


def delete_incompatible_models(model_paths: List[str]) -> Dict[str, any]:
    """
    Delete multiple incompatible model directories.

    Args:
        model_paths: List of paths to delete

    Returns:
        Dict with deletion results (success_count, failed_count, freed_size)
    """
    success_count = 0
    failed_count = 0
    freed_size = 0

    for path in model_paths:
        # Calculate size before deletion
        size = get_directory_size(path)

        if delete_model_directory(path):
            success_count += 1
            freed_size += size
        else:
            failed_count += 1

    return {
        "success_count": success_count,
        "failed_count": failed_count,
        "freed_size": freed_size,
        "freed_size_formatted": format_size(freed_size),
    }


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
