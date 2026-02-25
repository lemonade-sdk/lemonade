#!/usr/bin/env python3
"""
Script to update README.md with pinned apps from the marketplace.
Fetches apps.json and generates icons for pinned apps.
"""

import json
import re
import sys
from pathlib import Path
from urllib.request import urlopen
from urllib.error import URLError

# Configuration
APPS_JSON_URL = (
    "https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json"
)
README_PATH = Path(__file__).parent.parent / "README.md"

# Markers in README.md
START_MARKER = "<!-- MARKETPLACE_START -->"
END_MARKER = "<!-- MARKETPLACE_END -->"


def fetch_apps() -> list:
    """Fetch apps from the marketplace JSON."""
    try:
        with urlopen(APPS_JSON_URL, timeout=10) as response:
            data = json.loads(response.read().decode("utf-8"))
            return data.get("apps", [])
    except URLError as e:
        print(f"[ERROR] Failed to fetch apps.json: {e}")
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"[ERROR] Failed to parse apps.json: {e}")
        sys.exit(1)


def get_pinned_apps(apps: list, limit: int = 10) -> list:
    """Get pinned apps (already sorted by the marketplace build script)."""
    # Apps with pinned=true come first in apps.json (sorted by build.py)
    pinned = [app for app in apps if app.get("pinned", False)]
    return pinned[:limit]


def generate_markdown(apps: list) -> str:
    """Generate markdown for pinned apps."""
    if not apps:
        return ""

    # Generate icon links
    icons = []
    for app in apps:
        name = app.get("name", "Unknown")
        logo = app.get("logo", "")
        link = app.get("links", {}).get("guide") or app.get("links", {}).get("app", "#")

        if logo:
            icon_html = f'<a href="{link}" title="{name}"><img src="{logo}" alt="{name}" width="60" /></a>'
            icons.append(icon_html)

    # Join icons with spacing
    icons_html = "&nbsp;&nbsp;".join(icons)

    # Generate the full markdown block
    markdown = f"""
<p align="center">
  {icons_html}
</p>

<p align="center"><em><a href="https://lemonade-server.ai/marketplace">View all apps →</a></br>Want your app featured here? <a href="https://github.com/lemonade-sdk/marketplace">Just submit a marketplace PR!</a></em></p>
"""
    return markdown.strip()


def update_readme(markdown: str) -> bool:
    """Update README.md with the generated markdown."""
    if not README_PATH.exists():
        print(f"[ERROR] README.md not found at {README_PATH}")
        return False

    content = README_PATH.read_text(encoding="utf-8")

    # Check if markers exist
    if START_MARKER not in content or END_MARKER not in content:
        print("[ERROR] Markers not found in README.md")
        print(f"  Expected: {START_MARKER} ... {END_MARKER}")
        return False

    # Replace content between markers
    pattern = re.compile(
        rf"{re.escape(START_MARKER)}.*?{re.escape(END_MARKER)}", re.DOTALL
    )

    new_content = pattern.sub(f"{START_MARKER}\n{markdown}\n{END_MARKER}", content)

    if new_content == content:
        print("[INFO] No changes needed in README.md")
        return True

    README_PATH.write_text(new_content, encoding="utf-8")
    print(f"[OK] Updated README.md with {len(markdown.split('<a href'))-1} app icons")
    return True


def main():
    print("[INFO] Fetching apps from marketplace...")
    apps = fetch_apps()
    print(f"[INFO] Found {len(apps)} apps")

    pinned = get_pinned_apps(apps, limit=10)
    print(f"[INFO] Using {len(pinned)} pinned apps")

    markdown = generate_markdown(pinned)

    if not update_readme(markdown):
        sys.exit(1)


if __name__ == "__main__":
    main()
