# In conda environment of choice, run the following from genai/ folder:
# pip install -r docs/assets/mkdocs_requirements.txt

# Then run this script to publish the documentation to docs/docs/
# python docs/publish_website_docs.py

# Standard library imports for file, directory, regex, system, and subprocess operations
import os
import shutil
import re
import sys
import subprocess


def main():

    # Print the current working directory for debugging
    print("[INFO] Current working directory:", os.getcwd())

    # Define source and destination file paths
    src = "docs/server/README.md"
    dst = "docs/index.md"

    # Check if the source README exists; exit with error if not
    if not os.path.exists(src):
        print("[ERROR] docs/server/README.md not found!")
        sys.exit(1)

    # Read the source README, making necessary replacements
    with open(src, "r", encoding="utf-8") as f:
        readme_content = f.read()

    # Write the content to the destination index.md
    with open(dst, "w", encoding="utf-8") as f:
        f.write(readme_content)
    print("[INFO] Copied docs/server/README.md to docs/index.md.")

    # Read the just-written index.md and perform additional link fixes for website publishing
    print("[INFO] Fixing links in docs/index.md...")
    with open(dst, "r", encoding="utf-8") as f:
        content = f.read()

    # List of (pattern, replacement) tuples for fixing internal documentation links
    replacements = [
        (r"\(\./apps/README\.md\)", r"(./server/apps/README.md)"),
        (r"\(\./concepts\.md\)", r"(./server/concepts.md)"),
        (r"\(\./lemonade-server-cli\.md\)", r"(./server/lemonade-server-cli.md)"),
        (r"\(\./server_models\.md\)", r"(./server/server_models.md)"),
        (r"\(\./server_spec\.md\)", r"(./server/server_spec.md)"),
        (r"\(\./server_integration\.md\)", r"(./server/server_integration.md)"),
    ]
    for pattern, repl in replacements:
        content = re.sub(pattern, repl, content)

    # Write the fully processed content back to index.md
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)

    # Remove existing docs/docs if it exists
    if os.path.exists("docs/docs"):
        print("Removing ", os.path.abspath("docs/docs"))
        shutil.rmtree("docs/docs")

    # Build the documentation using mkdocs
    print("[INFO] Building documentation with mkdocs...")
    subprocess.run(["mkdocs", "build", "--clean"], check=True)

    # Move the generated site/ directory to docs/docs/, replacing it if it already exists
    print("[INFO] Moving site/ to docs/docs/...")

    # Check what mkdocs actually generated
    if os.path.exists(os.path.abspath("site/docs")):
        # If mkdocs generated site/docs/, move that content
        source_dir = os.path.abspath("site/docs")
    elif os.path.exists(os.path.abspath("site")):
        # If mkdocs generated site/, move that content
        source_dir = os.path.abspath("site")
    else:
        print("[ERROR] No site directory found after mkdocs build!")
        sys.exit(1)

    # Move the correct source directory
    shutil.move(source_dir, "docs/docs")
    print(f"[INFO] Moved {os.path.abspath(source_dir)} to docs/docs/")


if __name__ == "__main__":
    main()
