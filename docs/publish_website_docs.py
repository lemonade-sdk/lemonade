"""Build the docs site and stage it under docs/docs for the website branch.

Run from the repository root after installing docs/assets/docs_requirements.txt.
"""

import os
import platform
import shutil
import sys
import subprocess


def _get_venv_executable(name):
    """Resolve an executable from the active Python environment."""
    python_dir = os.path.dirname(sys.executable)
    if platform.system() == "Windows":
        return os.path.join(python_dir, "Scripts", f"{name}.exe")
    return os.path.join(python_dir, name)


def main():
    print("[INFO] Current working directory:", os.getcwd())

    # Zensical needs index.md, but the source page is maintained as README.md.
    src = "docs/README.md"
    dst = "docs/index.md"

    if not os.path.exists(src):
        print(f"[ERROR] {src} not found!")
        sys.exit(1)

    with open(src, "r", encoding="utf-8") as f:
        readme_content = f.read()

    with open(dst, "w", encoding="utf-8") as f:
        f.write(readme_content)
    print(f"[INFO] Copied {src} to {dst}.")

    if os.path.exists("docs/docs"):
        print("Removing ", os.path.abspath("docs/docs"))
        shutil.rmtree("docs/docs")

    print("[INFO] Building documentation with zensical...")
    zensical_exe = _get_venv_executable("zensical")
    print(f"[INFO] zensical path: {zensical_exe}")
    subprocess.run([zensical_exe, "build", "--clean"], check=True)

    print("[INFO] Moving site/ to docs/docs/...")

    # The website branch nests the docs build under site/docs; local builds use site/.
    if os.path.exists(os.path.abspath("site/docs")):
        source_dir = os.path.abspath("site/docs")
    elif os.path.exists(os.path.abspath("site")):
        source_dir = os.path.abspath("site")
    else:
        print("[ERROR] No site directory found after zensical build!")
        sys.exit(1)

    shutil.move(source_dir, "docs/docs")
    print(f"[INFO] Moved {os.path.abspath(source_dir)} to docs/docs/")


if __name__ == "__main__":
    main()
