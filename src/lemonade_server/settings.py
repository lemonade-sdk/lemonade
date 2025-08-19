import json
import os
from lemonade.cache import DEFAULT_CACHE_DIR

# Define the path for the user settings file, placing it in the cache directory
USER_SETTINGS_FILE = os.path.join(DEFAULT_CACHE_DIR, "user_settings.json")


def save_setting(key, value):
    """Save a setting to the user_settings.json file."""
    # Ensure the cache directory exists
    os.makedirs(DEFAULT_CACHE_DIR, exist_ok=True)

    settings = {}
    if os.path.exists(USER_SETTINGS_FILE):
        with open(USER_SETTINGS_FILE, "r") as f:
            try:
                settings = json.load(f)
            except json.JSONDecodeError:
                # If the file is empty or corrupt, start with a fresh dictionary
                pass

    settings[key] = value
    with open(USER_SETTINGS_FILE, "w") as f:
        json.dump(settings, f, indent=4)


def load_setting(key, default=None):
    """Load a setting from the user_settings.json file."""
    if not os.path.exists(USER_SETTINGS_FILE):
        return default

    with open(USER_SETTINGS_FILE, "r") as f:
        try:
            settings = json.load(f)
            return settings.get(key, default)
        except json.JSONDecodeError:
            # Return default if the file is empty or corrupt
            return default
