"""
Regression tests for the /web-app/ static asset route.

Tests the path-traversal security fix:
- Normal assets return 200
- Encoded traversal attempts are rejected with 403
- Nonexistent assets return 404
- Windows validation note (string-prefix check was broken on Windows)

Requires a running server (started by the installer or manually).
No inference backends are needed.

Usage:
    python server_webapp.py
"""

import os
import sys
import unittest
import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    parse_args,
    PORT,
    TIMEOUT_DEFAULT,
)


class WebAppAssetTests(ServerTestBase):
    """Tests for web-app static asset serving and path-traversal prevention."""

    additional_server_args = []

    @classmethod
    def setUpClass(cls):
        """Verify server is reachable. Do NOT pull any models."""
        super().setUpClass()

    def _is_webapp_available(self):
        """Check if the server has a web-app directory compiled in."""
        resp = requests.get(f"http://localhost:{PORT}/", timeout=TIMEOUT_DEFAULT)
        return resp.status_code == 200 and "<!doctype" in resp.text[:4096].lower()

    def test_001_normal_js_asset_returns_200(self):
        """A normal web-app JS asset should return 200."""
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        # renderer.bundle.js is a standard web-app asset
        resp = requests.get(
            f"http://localhost:{PORT}/renderer.bundle.js", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(resp.status_code, 200)
        self.assertIn("text/javascript", resp.headers.get("Content-Type", ""))
        self.assertGreater(
            len(resp.content), 0, "renderer.bundle.js should not be empty"
        )
        print("[OK] renderer.bundle.js returned 200 with correct content-type")

    def test_002_web_app_prefix_asset_returns_200(self):
        """A /web-app/ prefixed asset should return 200 (backwards compat)."""
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        resp = requests.get(
            f"http://localhost:{PORT}/web-app/renderer.bundle.js",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 200)
        self.assertGreater(len(resp.content), 0)
        print("[OK] /web-app/ prefixed path returned 200")

    def test_003_url_encoded_traversal_rejected(self):
        """
        URL-encoded path traversal (..%2f) should return 403 specifically.

        Uses backend_versions.json as a known existing file outside web-app.
        If the traversal check is broken, this would return 200 with the file content.
        """
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        # Try to access src/cpp/resources/backend_versions.json via traversal
        # From web-app directory: ../../resources/backend_versions.json
        # %2f = /, %2e = .
        resp = requests.get(
            f"http://localhost:{PORT}/web-app/..%2f..%2fresources%2fbackend_versions.json",
            timeout=TIMEOUT_DEFAULT,
        )
        # Must return 403 Forbidden, not 200 (file leaked) or 404 (file not found)
        self.assertEqual(
            resp.status_code,
            403,
            f"Encoded traversal to existing file should return 403, got {resp.status_code}",
        )
        print("[OK] URL-encoded traversal to backend_versions.json returned 403")

    def test_004_double_encoded_traversal_rejected(self):
        """Double-encoded traversal should be rejected with 403."""
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        # %252e = %2e (literal %2e), %252f = %2f (literal %2f)
        # Some proxies double-decode, so this tests defense-in-depth
        resp = requests.get(
            f"http://localhost:{PORT}/web-app/%252e%252e%252f%252e%252e%252fetc%252fpasswd",
            timeout=TIMEOUT_DEFAULT,
        )
        # This should either be 403 (traversal blocked) or 404 (file not found)
        # It should NOT be 200 (file leaked)
        self.assertNotEqual(
            resp.status_code, 200, "Double-encoded traversal should not return 200"
        )
        print(f"[OK] Double-encoded traversal returned {resp.status_code} (not 200)")

    def test_005_backslash_traversal_rejected(self):
        """Backslash-based traversal should be rejected with 403 on all platforms."""
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        # Some clients send backslashes as path separators
        resp = requests.get(
            f"http://localhost:{PORT}/web-app/..%5c..%5c..%5cetc%5cpasswd",
            timeout=TIMEOUT_DEFAULT,
        )
        # Should be 403 (traversal blocked) or 404 (not found)
        self.assertNotEqual(
            resp.status_code, 200, "Backslash traversal should not return 200"
        )
        print(f"[OK] Backslash traversal returned {resp.status_code} (not 200)")

    def test_006_nonexistent_asset_returns_404(self):
        """A nonexistent asset should return 404."""
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        resp = requests.get(
            f"http://localhost:{PORT}/nonexistent-file-xyz123.js",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 404)
        print("[OK] Nonexistent asset returned 404")

    def test_007_legitimate_filename_with_dots_allowed(self):
        """
        Legitimate filenames containing ".." as a substring should be allowed.

        This verifies we're checking path components, not substring matching.
        The old implementation using rel_str.find("..") would incorrectly
        reject filenames like "my..test..file.js".
        """
        if not self._is_webapp_available():
            self.skipTest("Web app not available on this server")

        resp = requests.get(
            f"http://localhost:{PORT}/web-app/my..test..file.js",
            timeout=TIMEOUT_DEFAULT,
        )
        # Should return 404 (file doesn't exist), not 403 (forbidden)
        self.assertEqual(
            resp.status_code,
            404,
            f"Filename with .. substring should return 404 (not found), not 403 (forbidden). Got {resp.status_code}",
        )
        print("[OK] Filename with .. substring returned 404 (not 403)")

    def test_008_windows_validation_note(self):
        """
        Windows path-separator validation note.

        The old string-prefix check compared candidate.string() against
        base.string() + "/" which is broken on Windows because
        std::filesystem::path::string() uses backslashes (e.g.
        C:\\web-app\\renderer.bundle.js won't start with C:\\web-app/).

        The fix uses std::filesystem::relative(candidate, base) which
        is platform-agnostic and works correctly on Windows, Linux, and macOS.

        Windows validation checklist (run manually on a Windows build):
        1. Build with: cmake --build --preset windows --target web-app
        2. Start lemond
        3. Verify: curl http://localhost:13305/renderer.bundle.js -> 200
        4. Verify: curl http://localhost:13305/web-app/..%2f..%2f..%2fetc%2fpasswd -> 403
        5. Verify: curl http://localhost:13305/nonexistent.js -> 404
        """
        self.skipTest("Windows validation must be done manually on a Windows build")


def main():
    parse_args()
    run_server_tests(
        WebAppAssetTests,
        description="WEB APP ASSET & SECURITY TESTS",
    )


if __name__ == "__main__":
    main()
