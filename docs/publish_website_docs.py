# From lemonade repo root, run the following:
# pip install -r docs/assets/docs_requirements.txt

# Then run this script to publish the documentation to docs/docs/
# python docs/publish_website_docs.py

# Standard library imports for file, directory, regex, system, and subprocess operations
import os
import platform
import shutil
import re
import sys
import subprocess


def _get_venv_executable(name):
    """Get an executable path from the venv based on the current Python interpreter."""
    python_dir = os.path.dirname(sys.executable)
    if platform.system() == "Windows":
        return os.path.join(python_dir, "Scripts", f"{name}.exe")
    else:
        return os.path.join(python_dir, name)


def main():

    # Print the current working directory for debugging
    print("[INFO] Current working directory:", os.getcwd())

    # Define source and destination file paths
    src = "docs/README.md"
    dst = "docs/index.md"

    # Check if the source README exists; exit with error if not
    if not os.path.exists(src):
        print(f"[ERROR] {src} not found!")
        sys.exit(1)

    # Read the source README
    with open(src, "r", encoding="utf-8") as f:
        readme_content = f.read()

    # Write the content to the destination index.md
    with open(dst, "w", encoding="utf-8") as f:
        f.write(readme_content)
    print(f"[INFO] Copied {src} to {dst}.")

    # Remove existing docs/docs if it exists
    if os.path.exists("docs/docs"):
        print("Removing ", os.path.abspath("docs/docs"))
        shutil.rmtree("docs/docs")

    # Build the documentation using zensical
    print("[INFO] Building documentation with zensical...")
    zensical_exe = _get_venv_executable("zensical")
    print(f"[INFO] zensical path: {zensical_exe}")
    subprocess.run([zensical_exe, "build", "--clean"], check=True)

    # Move the generated site/ directory to docs/docs/, replacing it if it already exists
    print("[INFO] Moving site/ to docs/docs/...")

    # Check what zensical actually generated
    if os.path.exists(os.path.abspath("site/docs")):
        # If zensical generated site/docs/, move that content
        source_dir = os.path.abspath("site/docs")
    elif os.path.exists(os.path.abspath("site")):
        # If zensical generated site/, move that content
        source_dir = os.path.abspath("site")
    else:
        print("[ERROR] No site directory found after zensical build!")
        sys.exit(1)

    # Move the correct source directory
    shutil.move(source_dir, "docs/docs")
    print(f"[INFO] Moved {os.path.abspath(source_dir)} to docs/docs/")


if __name__ == "__main__":
    main()
