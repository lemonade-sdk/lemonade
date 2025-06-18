import os
from typing import Optional
import socket
from huggingface_hub import model_info


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
