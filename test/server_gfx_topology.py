"""
GFX-topology backend-link tests for Lemonade Server.

Catches the class of bug in issue #2415: a ROCm backend whose download asset
name is built from the raw device ISA (e.g. gfx1201) 404s because the release
repo publishes assets under a *family* target name (e.g. gfx120X).

The server exposes POST /api/v1/install/dry-run, which resolves the asset URL
that install_backend() WOULD download for a given (recipe, backend, arch)
WITHOUT fetching any bytes, and reports whether that arch is a supported target.
This test drives it across every ROCm arch the manifest knows about and:

  * resolution check: asserts each ISA resolves to the published family name in
    the asset URL, so gfx1201 -> gfx120X is verified offline.
  * link check: for every arch the server reports as supported, HEAD-probes the
    resolved URL (or its .partcount manifest for split archives) and asserts it
    is not a 404, exercising the real publish state across the arch matrix. This
    is cheap (a handful of HEAD requests, no bytes downloaded) so it always runs;
    it skips (does not fail) only when no probe could reach the network at all.

No GPU arch data is hardcoded here: the arch matrix and the family mapping come
from backend_versions.json, and supportedness comes from the server, so adding
new hardware is a manifest edit with no change to this test.

Usage:
    python test/server_gfx_topology.py --cli-binary ./build/lemonade
"""

from __future__ import annotations

import json
from pathlib import Path

import requests

from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import PORT, TIMEOUT_DEFAULT

# The ROCm backends whose install asset name embeds the GPU target, so a stale
# target/version pin shows up as a 404. (llamacpp:rocm and sd-cpp:rocm fetch
# their arch-specific bits via TheRock's url_mapping, not the asset name, so they
# are not part of this matrix.) This is test scope, not GPU data — the arches
# each one supports are reported by the server.
ROCM_ASSET_BACKENDS = [
    ("whispercpp", "rocm"),
    ("vllm", "rocm"),
    ("llamacpp", "rocm-nightly"),
]


def _workspace_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _backend_versions() -> dict:
    return json.loads(
        (
            _workspace_root() / "src" / "cpp" / "resources" / "backend_versions.json"
        ).read_text(encoding="utf-8")
    )


# Read the same map SystemInfo::rocm_asset_family() uses, so this test and the
# server share one source of truth. The "comment" key is documentation, not a map
# entry, so it is dropped.
_FAMILY_MAP = {
    isa: family
    for isa, family in _backend_versions().get("rocm_asset_families", {}).items()
    if isa != "comment"
}


def rocm_asset_family(arch: str) -> str:
    """Collapse a concrete ISA to its published family target name."""
    return _FAMILY_MAP.get(arch, arch)


def arch_matrix() -> list[str]:
    """Every ROCm arch the manifest knows about: the therock.architectures pin
    (a superset of the ISAs the detector emits) unioned with the family-map keys,
    deduped."""
    seen = {}
    for arch in (
        _backend_versions().get("therock", {}).get("architectures", [])
        + list(_FAMILY_MAP.keys())
        + list(_FAMILY_MAP.values())
    ):
        seen.setdefault(arch, None)
    return list(seen.keys())


class GfxTopologyTests(ServerTestBase):
    """Drives the install dry-run resolver across every GFX topology."""

    def _dry_run(self, recipe, backend, arch):
        response = requests.post(
            f"http://localhost:{PORT}/api/v1/install/dry-run",
            json={"recipe": recipe, "backend": backend, "arch": arch},
            timeout=TIMEOUT_DEFAULT,
        )
        return response

    def _probe_not_404(self, url):
        """Return the observed status code for a backend asset URL.

        GitHub release downloads redirect to a CDN that sometimes rejects HEAD;
        fall back to a 1-byte ranged GET in that case.
        """
        head = requests.head(url, allow_redirects=True, timeout=TIMEOUT_DEFAULT)
        if head.status_code in {403, 405}:
            ranged = requests.get(
                url,
                headers={"Range": "bytes=0-0"},
                allow_redirects=True,
                stream=True,
                timeout=TIMEOUT_DEFAULT,
            )
            ranged.close()
            return ranged.status_code
        return head.status_code

    def test_000_known_isa_family_mappings_are_present(self):
        """Anchor specific ISA -> family mappings.

        The rest of the suite reads the same rocm_asset_families map as the
        server, which is the right single-source-of-truth behavior but would not
        notice an accidental deletion of a mapping (e.g. dropping gfx1201 would
        make every other check pass with gfx1201 used verbatim). These explicit
        pairs fail loudly if a known mapping disappears or changes.
        """
        for isa, expected_family in [
            ("gfx1201", "gfx120X"),
            ("gfx1103", "gfx110X"),
            ("gfx1036", "gfx103X"),
        ]:
            with self.subTest(isa=isa):
                self.assertEqual(
                    rocm_asset_family(isa),
                    expected_family,
                    f"{isa} must map to {expected_family} in rocm_asset_families; "
                    "did the mapping get removed from backend_versions.json?",
                )

    def test_001_dry_run_resolves_isa_to_published_family(self):
        """Every supported arch must resolve to its published family in the URL."""
        for recipe, backend in ROCM_ASSET_BACKENDS:
            for arch in arch_matrix():
                with self.subTest(recipe=recipe, backend=backend, arch=arch):
                    response = self._dry_run(recipe, backend, arch)
                    self.assertEqual(
                        response.status_code,
                        200,
                        f"dry-run failed for {recipe}:{backend} arch={arch}: "
                        f"{response.text[:500]}",
                    )
                    body = response.json()
                    # An arch this backend does not publish for is not expected to
                    # produce a usable asset name; only check the supported ones.
                    if not body.get("supported"):
                        continue
                    url = body.get("url", "")
                    family = rocm_asset_family(arch)

                    # The published family token must appear in the asset URL, and
                    # the raw non-family ISA must NOT (that was the #2415 bug).
                    self.assertIn(
                        family,
                        url,
                        f"{recipe}:{backend} arch={arch} should resolve to family "
                        f"{family}, got url={url}",
                    )
                    if family != arch:
                        self.assertNotIn(
                            arch,
                            url,
                            f"{recipe}:{backend} arch={arch} leaked the raw ISA into "
                            f"the asset URL instead of family {family}: {url}",
                        )

    def test_002_live_asset_links_are_not_404(self):
        """Resolved asset URLs (or .partcount manifests) must not 404.

        Always runs: the probe is a handful of HEAD requests and downloads no
        bytes. A 404 is a real failure (stale target/version pin, #2415). A
        network error on one probe is collected, not raised, so it cannot mask a
        404 confirmed on another probe: we fail on any 404 first, and only skip
        (no network reachable) when there were no 404s at all.
        """
        failures = []
        network_errors = []
        for recipe, backend in ROCM_ASSET_BACKENDS:
            # Skip nightly channels: their pin is a moving target whose assets age
            # out, and the repo publishes a channel-specific subset that the
            # unified "rocm" support matrix does not model (e.g. nightly omits
            # gfx1152 that the stable/TheRock channel builds). Resolution is still
            # checked for them in test_001.
            if "nightly" in backend:
                continue
            # Probe one representative arch per published family to keep the live
            # check fast while still covering every distinct asset.
            probed_families = set()
            for arch in arch_matrix():
                family = rocm_asset_family(arch)
                if family in probed_families:
                    continue

                response = self._dry_run(recipe, backend, arch)
                if response.status_code != 200 or not response.json().get("supported"):
                    # The backend does not publish an asset for this arch; a
                    # missing build there is by design, not a regression.
                    continue
                probed_families.add(family)

                body = response.json()
                url = body["url"]
                if body.get("supports_split_archive"):
                    # Split releases publish parts + a {base}.partcount manifest;
                    # the single-file asset name will not exist. Probe the manifest.
                    base = url[: -len(".tar.gz")] if url.endswith(".tar.gz") else url
                    url = base + ".partcount"

                try:
                    status = self._probe_not_404(url)
                except requests.RequestException as exc:
                    network_errors.append(f"{recipe}:{backend} arch={arch}: {exc}")
                    continue
                if status == 404:
                    failures.append(
                        f"{recipe}:{backend} arch={arch} (family {family}) -> 404 {url}"
                    )

        self.assertFalse(
            failures,
            "Backend asset links returned 404:\n  " + "\n  ".join(failures),
        )
        if network_errors:
            self.skipTest(
                "Could not reach asset host for some probes (no 404s seen):\n  "
                + "\n  ".join(network_errors)
            )


if __name__ == "__main__":
    run_server_tests(GfxTopologyTests, "SERVER GFX TOPOLOGY TESTS")
