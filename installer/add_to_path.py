import winreg
import argparse


def add_to_path(directory_to_add):
    """
    Adds a directory to the beginning of the user Path, or
    moves it to the beginning if it already exists in the Path.

    Args:
        directory_to_add (str): Directory path to add to the Path

    Returns:
        bool: True if successful, False otherwise
    """
    try:
        # Open the Environment key in HKEY_CURRENT_USER
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            "Environment",
            0,
            winreg.KEY_READ | winreg.KEY_WRITE,
        )

        # Get the current Path value
        try:
            # Try to get the current Path value
            # If the Path env var exists but it is empty, it will return an empty string
            current_path, _ = winreg.QueryValueEx(key, "Path")
        except FileNotFoundError:
            # If the Path env var doesn't exist yet, it will raise a FileNotFoundError
            # In this case ONLY, it is safe to set the current path to an empty string
            current_path = ""
        except Exception as e:
            # If anything else goes wrong, print the error and exit
            # We don't want to risk corrupting the registry
            print(f"Error getting current Path: {e}")
            exit(1)

        # Split the Path into individual directories
        path_items = [
            item for item in current_path.split(";") if item
        ]  # Remove empty entries

        # Check if directory is already in Path
        if directory_to_add in path_items:
            # Remove it from its current position
            path_items.remove(directory_to_add)
            print(f"- {directory_to_add} was already in Path, moving to the beginning")
        else:
            print(f"- Adding {directory_to_add} to the beginning of Path")

        # Add the directory to the beginning of Path
        path_items.insert(0, directory_to_add)

        # Join the items back together
        new_path = ";".join(path_items)

        # Write the new Path back to registry
        winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, new_path)
        winreg.CloseKey(key)

        print("- Successfully updated user Path")
        return True

    except Exception as e:
        print(f"Error updating Path: {e}")
        return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Add a directory to the beginning of the user Path"
    )
    parser.add_argument("directory", help="Directory path to add to Path")
    args = parser.parse_args()

    add_to_path(args.directory)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
