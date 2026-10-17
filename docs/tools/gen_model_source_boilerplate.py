#!/usr/bin/env python3
"""Generate model-source docs from the live `/internal/model-sources` payload.

Every fact these regions carry is a compiled descriptor field: registries declare
theirs in `src/cpp/server/models/registry_descriptors.cpp`, catalogs in their
`CatalogDescriptor`. The payload is served by `ModelManager::model_sources()`, so
the docs cannot claim a token, an endpoint or an environment variable that the
running server does not have.

A CI step runs it with ``--check`` and fails if any committed region drifts.

Usage:
    python docs/tools/gen_model_source_boilerplate.py [--lemond PATH] [--check]

Regions are the same ``BEGIN/END GENERATED`` markers used by
``gen_backend_boilerplate.py``, whose ``Lemond`` harness this reuses.

Rows whose ``CatalogDescriptor.host_specific`` is true name an installed backend
or a configured cloud provider, so they vary by machine and are never written
into a committed page.
"""

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gen_backend_boilerplate import (  # noqa: E402
    Lemond,
    apply_sections,
    find_lemond,
    md_escape,
)


class Sources(Lemond):
    """The backend-docs harness plus the one endpoint this generator reads."""

    def model_sources(self) -> dict:
        return json.loads(self._get("/internal/model-sources", timeout=30))


def _list(items, joiner):
    if not items:
        return "none"
    if len(items) == 1:
        return items[0]
    return ", ".join(items[:-1]) + f" {joiner} " + items[-1]


def _codes(items, joiner):
    return _list([f"`{item}`" for item in items], joiner)


def _yes_no(value) -> str:
    return "yes" if value else "no"


def render_registry_tokens(registries) -> str:
    return _codes([row["token"] for row in registries], "or")


def render_local_origin_tokens(tokens) -> str:
    return _codes(tokens, "or")


def render_pull_source_values(values) -> str:
    return _codes(values, "or")


def render_registry_table(registries) -> str:
    rows = [
        "| Registry | Endpoint | Schemes | Environment | Cache prefix "
        "| Revision semantics | Staleness basis | Search |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for reg in registries:
        env = _codes([var["name"] for var in reg["env"]], "and")
        endpoint = (
            f"`{reg['endpoint_default']}`" if reg["endpoint_default"] else "its own API"
        )
        scheme = f"`{reg['scheme']}`" if reg["scheme"] else "—"
        rows.append(
            f"| `{reg['token']}` | {endpoint} | {scheme} | {env} "
            f"| `{md_escape(reg['cache_dir_prefix'])}` "
            f"| {reg['revision_semantics']} | {reg['staleness_basis']} "
            f"| {_yes_no(reg['search'])} |"
        )
    return "\n".join(rows)


def render_registry_notes(registries) -> str:
    blocks = []
    for reg in registries:
        lines = [f"**{reg['display_name']}**", "", reg["summary"], ""]
        for note in reg["notes"]:
            lines.append(f"- {note}")
        if reg["example"]:
            lines.append(f"- URL form: `{reg['example']}`")
        lines.append(f"- Default revision: `{reg['default_revision']}`.")
        blocks.append("\n".join(lines))
    return "\n\n".join(blocks)


def render_catalog_table(catalogs) -> str:
    shipped = [row for row in catalogs if not row["host_specific"]]
    rows = [
        "| Catalog | Entries come from | Required | Config key |",
        "| --- | --- | --- | --- |",
    ]
    for row in shipped:
        key = f"`{row['config_key']}`" if row["config_key"] else "—"
        rows.append(
            f"| {row['title']} | {row['summary']} | {_yes_no(row['required'])} | {key} |"
        )
    return "\n".join(rows)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--lemond", help="Path to the built lemond binary")
    ap.add_argument(
        "--check", action="store_true", help="Fail if docs are stale; do not write"
    )
    args = ap.parse_args()

    with Sources(find_lemond(args.lemond)) as server:
        payload = server.model_sources()

    registries = payload["registries"]
    catalogs = payload["catalogs"]
    if not registries or not catalogs:
        sys.exit(
            "/internal/model-sources returned no registries or no catalogs; "
            "a source with no descriptor must fail loudly rather than empty a page"
        )

    targets = {
        REPO_ROOT
        / "docs"
        / "dev"
        / "model-registries.md": {
            "model-registries": render_registry_table(registries),
            "model-registries-notes": render_registry_notes(registries),
            "remote-registry-tokens": render_registry_tokens(registries),
            "local-origin-tokens": render_local_origin_tokens(
                payload["local_origin_tokens"]
            ),
            "pull-source-values": render_pull_source_values(
                payload["pull_source_values"]
            ),
        },
        REPO_ROOT
        / "docs"
        / "guide"
        / "configuration"
        / "custom-models.md": {
            "model-catalogs": render_catalog_table(catalogs),
            "remote-registry-tokens": render_registry_tokens(registries),
        },
        REPO_ROOT
        / "docs"
        / "guide"
        / "cli.md": {
            "cli-pull-source-values": render_registry_tokens(registries),
        },
    }

    stale = []
    for path, sections in targets.items():
        rel = path.relative_to(REPO_ROOT)
        if not path.exists():
            sys.exit(f"{rel} is missing")
        updated = apply_sections(path.read_text(), sections)
        if args.check:
            if path.read_text() != updated:
                stale.append(str(rel))
        else:
            path.write_text(updated)
            print(f"Wrote {rel}")

    if args.check:
        if stale:
            sys.exit(
                "Stale generated docs: "
                + ", ".join(stale)
                + "\nRun: python docs/tools/gen_model_source_boilerplate.py"
            )
        print("All model source docs are up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
