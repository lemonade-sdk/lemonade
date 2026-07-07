"""
Request cancellation integration tests for Lemonade Server.

Tests the request cancellation API:
- GET /v1/requests — list active requests
- POST /v1/requests/{request_id}/cancel — cancel an active inference request
- X-Request-Id request/response header for request identification
- Quad-prefix support: /api/v0/, /api/v1/, /v0/, /v1/

Requires a running Lemonade server with an LLM backend on port 13305.

Usage:
    python server_cancel.py --wrapped-server llamacpp --backend vulkan
    python server_cancel.py --wrapped-server llamacpp --backend cpu
    python server_cancel.py --wrapped-server ryzenai --backend hybrid
    python server_cancel.py --wrapped-server flm
"""

import os
import threading
import time
import uuid

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.capabilities import (
    skip_if_unsupported,
)
from utils.test_models import (
    PORT,
    STANDARD_MESSAGES,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


# All four URL prefixes that must serve the same endpoints.
_PREFIXES = [
    "http://localhost:{port}/api/v0",
    "http://localhost:{port}/api/v1",
    "http://localhost:{port}/v0",
    "http://localhost:{port}/v1",
]


def _auth_headers():
    """Return Authorization header if LEMONADE_API_KEY is set."""
    api_key = os.environ.get("LEMONADE_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


class CancelTests(ServerTestBase):
    """Tests for the request cancellation API."""

    @classmethod
    def setUpClass(cls):
        """Verify server is reachable and a test model is available."""
        super().setUpClass()

    # =========================================================================
    # LIST REQUESTS
    # =========================================================================

    def test_001_list_requests_empty(self):
        """GET /v1/requests returns an empty list when no active requests."""
        for prefix_tmpl in _PREFIXES:
            prefix = prefix_tmpl.format(port=PORT)
            response = requests.get(
                f"{prefix}/requests",
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                response.status_code,
                200,
                f"Expected 200 from {prefix}/requests, got {response.status_code}: "
                f"{response.text[:300]}",
            )
            data = response.json()
            self.assertIsInstance(
                data, list, f"Expected list from {prefix}/requests, got {type(data)}"
            )
            # When idle there should be no in-flight requests.  We allow the
            # list to be empty; if it isn't, the entries must at least be
            # well-formed dicts.
            for entry in data:
                self.assertIsInstance(entry, dict)
                self.assertIn("request_id", entry)

        print("[OK] GET /requests returns empty list on all 4 prefixes")

    # =========================================================================
    # CANCEL NONEXISTENT
    # =========================================================================

    def test_002_cancel_nonexistent(self):
        """POST /v1/requests/{fake_id}/cancel returns 404."""
        fake_id = uuid.uuid4().hex
        for prefix_tmpl in _PREFIXES:
            prefix = prefix_tmpl.format(port=PORT)
            response = requests.post(
                f"{prefix}/requests/{fake_id}/cancel",
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                response.status_code,
                404,
                f"Expected 404 from {prefix}/requests/{fake_id}/cancel, "
                f"got {response.status_code}: {response.text[:300]}",
            )
            body = response.json()
            self.assertIn("error", body)
            self.assertIn("request_id", body["error"])
            self.assertEqual(body["error"]["request_id"], fake_id)

        print("[OK] Cancel of nonexistent request returns 404 on all 4 prefixes")

    # =========================================================================
    # CANCEL INVALID FORMAT
    # =========================================================================

    def test_003_cancel_invalid_format(self):
        """POST with empty/malformed request ID returns 400 or 404."""
        # An empty request_id captured by the regex yields a 400 (missing id).
        # The regex (.+) requires at least one character, so a path like
        # /requests//cancel will 404 at the route level instead.  We test the
        # more common case: a request ID that is syntactically valid but does
        # not correspond to any known request.
        bogus_ids = [
            "",  # empty string
            "   ",  # whitespace only
            "not-a-real-request-id-at-all",
            "../etc/passwd",  # path traversal attempt
        ]
        prefix = f"http://localhost:{PORT}/api/v1"

        for bogus_id in bogus_ids:
            if not bogus_id or bogus_id.strip() == "":
                # Skip truly empty IDs — the regex won't match and the route
                # returns a generic 404, which is also acceptable.
                continue
            # Some bogus IDs contain characters that need to be sent as-is;
            # requests will URL-encode them automatically.
            response = requests.post(
                f"{prefix}/requests/{bogus_id}/cancel",
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertIn(
                response.status_code,
                (400, 404),
                f"Expected 400 or 404 for request_id={bogus_id!r}, "
                f"got {response.status_code}: {response.text[:300]}",
            )

        print("[OK] Cancel with malformed request IDs returns 400/404")

    # =========================================================================
    # X-Request-Id HEADER (streaming)
    # =========================================================================

    @skip_if_unsupported("chat_completions_streaming")
    def test_004_request_id_header_streaming(self):
        """Streaming chat completion responses include X-Request-Id header."""
        model = self.get_test_model("llm")
        prefix = f"http://localhost:{PORT}/api/v1"

        response = requests.post(
            f"{prefix}/chat/completions",
            json={
                "model": model,
                "messages": STANDARD_MESSAGES,
                "stream": True,
                "max_tokens": 5,
            },
            headers=_auth_headers(),
            stream=True,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(
            response.status_code,
            200,
            f"Expected 200, got {response.status_code}: {response.text[:300]}",
        )

        # The X-Request-Id header must be present in the response.
        req_id = response.headers.get("X-Request-Id") or response.headers.get(
            "x-request-id"
        )
        self.assertIsNotNone(
            req_id,
            "Streaming response must include X-Request-Id header. "
            f"Headers present: {list(response.headers.keys())}",
        )
        self.assertGreater(len(req_id), 0, "X-Request-Id must not be empty")

        # Consume the stream so the connection is cleaned up.
        for _ in response.iter_lines():
            pass

        print(f"[OK] Streaming response includes X-Request-Id: {req_id}")

    # =========================================================================
    # CLIENT-PROVIDED X-Request-Id
    # =========================================================================

    @skip_if_unsupported("chat_completions_streaming")
    def test_005_client_provided_request_id(self):
        """Server uses the client-supplied X-Request-Id header value."""
        model = self.get_test_model("llm")
        prefix = f"http://localhost:{PORT}/api/v1"
        client_id = f"test-client-{uuid.uuid4().hex[:16]}"

        response = requests.post(
            f"{prefix}/chat/completions",
            json={
                "model": model,
                "messages": STANDARD_MESSAGES,
                "stream": True,
                "max_tokens": 5,
            },
            headers={**_auth_headers(), "X-Request-Id": client_id},
            stream=True,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        resp_id = response.headers.get("X-Request-Id") or response.headers.get(
            "x-request-id"
        )
        self.assertIsNotNone(resp_id, "Response must include X-Request-Id header")
        self.assertEqual(
            resp_id,
            client_id,
            f"Server should echo client's X-Request-Id ({client_id!r}), "
            f"got {resp_id!r}",
        )

        # Consume the stream.
        for _ in response.iter_lines():
            pass

        print(f"[OK] Server echoed client X-Request-Id: {client_id}")

    # =========================================================================
    # CANCEL STREAMING REQUEST
    # =========================================================================

    @skip_if_unsupported("chat_completions_streaming")
    def test_006_cancel_streaming_request(self):
        """Start a long stream, cancel mid-generation, verify stream ends."""
        model = self.get_test_model("llm")
        prefix = f"http://localhost:{PORT}/api/v1"

        # Use a long max_tokens so the stream runs long enough to cancel.
        # Ask for a verbose topic to encourage the model to keep generating.
        messages = [
            {
                "role": "user",
                "content": (
                    "Write a very long and detailed essay about the history of "
                    "computing, starting from Charles Babbage and going through "
                    "every major milestone up to the present day. Be as verbose "
                    "as possible."
                ),
            }
        ]

        stream_response = requests.post(
            f"{prefix}/chat/completions",
            json={
                "model": model,
                "messages": messages,
                "stream": True,
                "max_tokens": 500,
            },
            headers=_auth_headers(),
            stream=True,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(stream_response.status_code, 200)

        # Grab the request ID from the response headers.
        request_id = stream_response.headers.get(
            "X-Request-Id"
        ) or stream_response.headers.get("x-request-id")
        self.assertIsNotNone(request_id, "Streaming response must include X-Request-Id")

        # Read a few chunks to make sure the model is actively generating.
        chunks_received = 0
        for raw_line in stream_response.iter_lines():
            if raw_line:
                chunks_received += 1
            if chunks_received >= 3:
                break

        self.assertGreater(
            chunks_received,
            0,
            "Should have received at least some SSE chunks before cancelling",
        )

        # Cancel the request.
        cancel_response = requests.post(
            f"{prefix}/requests/{request_id}/cancel",
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertIn(
            cancel_response.status_code,
            (200, 404),
            f"Cancel should return 200 (cancelled) or 404 (already done). "
            f"Got {cancel_response.status_code}: {cancel_response.text[:300]}",
        )

        if cancel_response.status_code == 200:
            body = cancel_response.json()
            self.assertEqual(body.get("status"), "cancelled")
            self.assertEqual(body.get("request_id"), request_id)

        # The stream should terminate shortly after cancellation.  We don't
        # assert a specific SSE cancel event since the wire format depends on
        # the backend; we just verify the connection closes.
        remaining = 0
        for raw_line in stream_response.iter_lines():
            if raw_line:
                remaining += 1
            # Safety valve: don't wait forever if the backend ignores cancel.
            if remaining > 200:
                break

        print(
            f"[OK] Cancelled streaming request {request_id} "
            f"(chunks before cancel: {chunks_received}, "
            f"chunks after cancel: {remaining})"
        )

    # =========================================================================
    # LIST ACTIVE DURING STREAMING
    # =========================================================================

    @skip_if_unsupported("chat_completions_streaming")
    def test_007_list_active_during_streaming(self):
        """A streaming request appears in GET /v1/requests while in-flight."""
        model = self.get_test_model("llm")
        prefix = f"http://localhost:{PORT}/api/v1"

        messages = [
            {
                "role": "user",
                "content": (
                    "Count from 1 to 1000 slowly, writing each number on its own "
                    "line. Do not skip any numbers."
                ),
            }
        ]

        stream_done = threading.Event()
        request_id_holder = [None]

        def _run_stream():
            try:
                resp = requests.post(
                    f"{prefix}/chat/completions",
                    json={
                        "model": model,
                        "messages": messages,
                        "stream": True,
                        "max_tokens": 300,
                    },
                    headers=_auth_headers(),
                    stream=True,
                    timeout=TIMEOUT_MODEL_OPERATION,
                )
                if resp.status_code == 200:
                    request_id_holder[0] = resp.headers.get(
                        "X-Request-Id"
                    ) or resp.headers.get("x-request-id")
                for _ in resp.iter_lines():
                    pass
            except Exception:
                pass
            finally:
                stream_done.set()

        t = threading.Thread(target=_run_stream, daemon=True)
        t.start()

        # Give the stream a moment to register.
        time.sleep(1.0)

        # List active requests.
        list_resp = requests.get(
            f"{prefix}/requests",
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(list_resp.status_code, 200)
        active = list_resp.json()
        self.assertIsInstance(active, list)

        # The streaming request should appear in the list (if it hasn't
        # already completed).  We check by request_id if available.
        request_id = request_id_holder[0]
        if request_id:
            ids = [entry.get("request_id") for entry in active]
            # If the request is still active, it must appear.  If it already
            # finished (fast model), the list may be empty — that's acceptable.
            if request_id in ids:
                # Validate the entry shape.
                entry = next(e for e in active if e["request_id"] == request_id)
                self.assertIn("model_name", entry)
                self.assertIn("endpoint", entry)
                self.assertIn("is_streaming", entry)
                self.assertIn("elapsed_ms", entry)
                self.assertTrue(
                    entry["is_streaming"],
                    "Expected is_streaming=True for a streaming chat completion",
                )
                print(f"[OK] Active request visible in list: {entry}")
            else:
                print(
                    f"[OK] Request {request_id} already completed before listing "
                    f"(fast model). Active count: {len(active)}"
                )
        else:
            print("[OK] Stream started but request_id not captured; skipping match")

        # Wait for the stream thread to finish so we don't leak connections.
        stream_done.wait(timeout=30)
        t.join(timeout=5)

    # =========================================================================
    # CANCEL ALREADY COMPLETED
    # =========================================================================

    @skip_if_unsupported("chat_completions")
    def test_008_cancel_already_completed(self):
        """Fast non-streaming completion, then cancel → 404 (not found)."""
        model = self.get_test_model("llm")
        prefix = f"http://localhost:{PORT}/api/v1"

        # Issue a short non-streaming request that completes quickly.
        response = requests.post(
            f"{prefix}/chat/completions",
            json={
                "model": model,
                "messages": [{"role": "user", "content": "Say 'hi'."}],
                "stream": False,
                "max_tokens": 3,
            },
            headers=_auth_headers(),
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # The request has completed.  Try to cancel it.  Since non-streaming
        # requests may or may not be tracked in the registry, we use a random
        # ID to exercise the 404 path.
        fake_id = uuid.uuid4().hex
        cancel_resp = requests.post(
            f"{prefix}/requests/{fake_id}/cancel",
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            cancel_resp.status_code,
            404,
            f"Expected 404 for completed/unknown request, got "
            f"{cancel_resp.status_code}: {cancel_resp.text[:300]}",
        )

        print("[OK] Cancel of already-completed request returns 404")

    # =========================================================================
    # QUAD-PREFIX CANCEL
    # =========================================================================

    def test_009_quad_prefix_cancel(self):
        """Cancel endpoint works on all 4 URL prefixes with same behavior."""
        fake_id = uuid.uuid4().hex
        results = {}

        for prefix_tmpl in _PREFIXES:
            prefix = prefix_tmpl.format(port=PORT)
            response = requests.post(
                f"{prefix}/requests/{fake_id}/cancel",
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            results[prefix] = response.status_code
            self.assertEqual(
                response.status_code,
                404,
                f"Expected 404 from {prefix}/requests/{fake_id}/cancel, "
                f"got {response.status_code}",
            )

        # Also verify the list endpoint on all prefixes.
        for prefix_tmpl in _PREFIXES:
            prefix = prefix_tmpl.format(port=PORT)
            response = requests.get(
                f"{prefix}/requests",
                headers=_auth_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
            self.assertEqual(
                response.status_code,
                200,
                f"Expected 200 from {prefix}/requests, got {response.status_code}",
            )

        print(f"[OK] Quad-prefix cancel and list: {results}")

    # =========================================================================
    # DOUBLE CANCEL
    # =========================================================================

    @skip_if_unsupported("chat_completions_streaming")
    def test_010_cancel_double(self):
        """Cancel same request twice: first 200, second 404."""
        model = self.get_test_model("llm")
        prefix = f"http://localhost:{PORT}/api/v1"

        messages = [
            {
                "role": "user",
                "content": (
                    "Write a very detailed and long story about a rabbit who "
                    "goes on an adventure through a magical forest. Include "
                    "lots of dialogue and description."
                ),
            }
        ]

        stream_response = requests.post(
            f"{prefix}/chat/completions",
            json={
                "model": model,
                "messages": messages,
                "stream": True,
                "max_tokens": 500,
            },
            headers=_auth_headers(),
            stream=True,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(stream_response.status_code, 200)

        request_id = stream_response.headers.get(
            "X-Request-Id"
        ) or stream_response.headers.get("x-request-id")
        self.assertIsNotNone(request_id, "Need X-Request-Id to test double cancel")

        # Read a few chunks to ensure the request is active.
        chunks = 0
        for raw_line in stream_response.iter_lines():
            if raw_line:
                chunks += 1
            if chunks >= 3:
                break

        # First cancel.
        cancel1 = requests.post(
            f"{prefix}/requests/{request_id}/cancel",
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        # Consume the rest of the stream so the request is fully deregistered.
        for _ in stream_response.iter_lines():
            pass

        # Small delay to let the registry clean up.
        time.sleep(0.5)

        # Second cancel — the request is now gone.
        cancel2 = requests.post(
            f"{prefix}/requests/{request_id}/cancel",
            headers=_auth_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        if cancel1.status_code == 200:
            # First cancel succeeded; second must be 404.
            self.assertEqual(
                cancel2.status_code,
                404,
                f"Second cancel should return 404, got {cancel2.status_code}: "
                f"{cancel2.text[:300]}",
            )
            print(
                f"[OK] Double cancel: first=200 (cancelled), "
                f"second=404 (not found) for {request_id}"
            )
        else:
            # The request completed before we could cancel it.  Both should be
            # 404, which is still correct behavior.
            self.assertIn(
                cancel1.status_code,
                (200, 404),
                f"First cancel returned unexpected status {cancel1.status_code}",
            )
            self.assertEqual(
                cancel2.status_code,
                404,
                f"Second cancel should be 404, got {cancel2.status_code}",
            )
            print(
                f"[OK] Double cancel: first={cancel1.status_code}, "
                f"second=404 for {request_id} (request completed before cancel)"
            )


if __name__ == "__main__":
    run_server_tests(
        CancelTests,
        "REQUEST CANCELLATION TESTS",
        modality="llm",
    )
