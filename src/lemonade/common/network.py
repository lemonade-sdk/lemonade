import os
from typing import Optional
import socket
from huggingface_hub import model_info, snapshot_download
from huggingface_hub.errors import LocalEntryNotFoundError


def is_offline():
    """
    Check if the system is offline by attempting to connect to huggingface.co.

    Returns:
        bool: True if the system is offline (cannot connect to huggingface.co),
              False otherwise.
    """
    if os.environ.get("LEMONADE_OFFLINE"):
        return True
    try:
        socket.gethostbyname("huggingface.co")
        return False
    except socket.gaierror:
        return True


def get_base_model(checkpoint: str) -> Optional[str]:
    """
    Get the base model information for a given checkpoint from the Hugging Face Hub.
    Will auto-detect if we're offline and skip the network call in that case.

    Args:
        checkpoint: The model checkpoint to query

    Returns:
        The base model name if found, or None if not found or error occurs
    """
    # Skip network call in offline mode
    if is_offline():
        return None

    try:
        info = model_info(checkpoint)
        if info.cardData and "base_model" in info.cardData:
            if info.cardData["base_model"] is not None:
                # This is a derived model
                return info.cardData["base_model"]
            else:
                # This is itself a base model
                return [checkpoint]
    except Exception:  # pylint: disable=broad-except
        pass
    return None


def _symlink_safe_snapshot_download(repo_id, **kwargs):
    """
    Custom snapshot download with retry logic for Windows symlink privilege errors.
    """

    for attempt in range(2):
        try:
            return snapshot_download(repo_id=repo_id, **kwargs)
        except OSError as e:
            if (
                hasattr(e, "winerror")
                and e.winerror == 1314  # pylint: disable=no-member
                and attempt < 1
            ):
                continue
            raise


def custom_snapshot_download(repo_id, do_not_upgrade=False, **kwargs):
    """
    Custom snapshot download with:
        1) retry logic for Windows symlink privilege errors.
        2) do_not_upgrade allows the caller to prioritize a local copy
            of the model over an upgraded remote copy.
    """

    if do_not_upgrade:
        try:
            # Prioritize the local model, if available
            return _symlink_safe_snapshot_download(
                repo_id, local_files_only=True, **kwargs
            )
        except LocalEntryNotFoundError:
            # LocalEntryNotFoundError means there was no local model, at this point
            # we'll accept a remote model
            return _symlink_safe_snapshot_download(
                repo_id, local_files_only=False, **kwargs
            )
    else:
        return _symlink_safe_snapshot_download(repo_id, **kwargs)
