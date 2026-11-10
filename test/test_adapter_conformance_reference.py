"""R15: the greedy reference check in tools/adapter_conformance.py.

python3 -m unittest test/test_adapter_conformance_reference.py
"""

import hashlib
import os
import sys
import unittest

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
)
import adapter_conformance as ac  # noqa: E402

ANSWER = "A lighthouse warns ships of danger."
SHA = hashlib.sha256(ANSWER.encode("utf-8")).hexdigest()
REF = {
    "prompt": "In one sentence, what is a lighthouse for?",
    "max_tokens": 16,
    "sha256": SHA,
}


def fake_call(reply_text=ANSWER, error=None):
    calls = []

    def call(base, path, body=None, timeout=1900):
        calls.append((base, path, body))
        if error is not None:
            raise error
        return {"choices": [{"message": {"content": reply_text}}]}

    call.calls = calls
    return call


class ReferenceCheck(unittest.TestCase):
    def setUp(self):
        self._call = ac.call

    def tearDown(self):
        ac.call = self._call

    def test_matching_answer_passes(self):
        ac.call = fake_call()
        ok, detail = ac.reference_check("http://x", "m1", REF)
        self.assertTrue(ok)
        self.assertIn(SHA[:12], detail)

    def test_request_is_greedy_and_bounded(self):
        ac.call = fake_call()
        ac.reference_check("http://x", "m1", REF)
        base, path, body = ac.call.calls[0]
        self.assertEqual((base, path), ("http://x", "/api/v1/chat/completions"))
        self.assertEqual(body["model"], "m1")
        self.assertEqual(body["max_tokens"], 16)
        self.assertEqual(body["temperature"], 0)
        self.assertEqual(body["messages"], [{"role": "user", "content": REF["prompt"]}])
        self.assertFalse(body.get("stream", False))

    def test_different_answer_fails_and_shows_both_hashes(self):
        ac.call = fake_call("A lighthouse guides ships.")
        ok, detail = ac.reference_check("http://x", "m1", REF)
        got = hashlib.sha256("A lighthouse guides ships.".encode()).hexdigest()
        self.assertFalse(ok)
        self.assertIn(SHA[:12], detail)
        self.assertIn(got[:12], detail)

    def test_expected_hash_is_compared_case_insensitively(self):
        ac.call = fake_call()
        ok, _ = ac.reference_check("http://x", "m1", dict(REF, sha256=SHA.upper()))
        self.assertTrue(ok)

    def test_whitespace_is_part_of_the_answer(self):
        ac.call = fake_call(ANSWER + "\n")
        ok, _ = ac.reference_check("http://x", "m1", REF)
        self.assertFalse(ok)

    def test_call_failure_is_a_failed_check_not_an_exception(self):
        ac.call = fake_call(error=OSError("connection refused"))
        ok, detail = ac.reference_check("http://x", "m1", REF)
        self.assertFalse(ok)
        self.assertIn("connection refused", detail)

    def test_malformed_reference_fails_without_calling(self):
        for bad in (
            {},
            {"prompt": "p", "max_tokens": 4},
            {"prompt": "p", "max_tokens": 0, "sha256": SHA},
            {"prompt": "", "max_tokens": 4, "sha256": SHA},
            {"prompt": "p", "max_tokens": 4, "sha256": "abc"},
            {"prompt": "p", "max_tokens": "4", "sha256": SHA},
            {"prompt": "p", "max_tokens": 4, "sha256": "g" * 64},
        ):
            ac.call = fake_call()
            ok, detail = ac.reference_check("http://x", "m1", bad)
            self.assertFalse(ok, bad)
            self.assertIn("reference", detail, bad)
            self.assertEqual(ac.call.calls, [], bad)

    def test_reply_without_content_fails(self):
        ac.call = lambda base, path, body=None, timeout=1900: {"choices": []}
        ok, _ = ac.reference_check("http://x", "m1", REF)
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
