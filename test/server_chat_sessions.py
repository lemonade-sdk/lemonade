"""
Persistent `lemonade chat` session tests for one-shot use.

Runs the lemonade CLI against the mock OpenAI-style server from
server_chat_oneshot.py, with the config directory pointed at a temporary
folder, so no real server, model or user data is involved.

Usage:
    python server_chat_sessions.py
    python server_chat_sessions.py --cli-binary /path/to/lemonade
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

try:
    import fcntl
    import pty
except ImportError:  # Windows
    fcntl = None
    pty = None

from server_chat_oneshot import MODEL, MockHandler, MockState
from utils.test_models import get_default_cli_binary

_config = {"cli_binary": None}


class ChatSessionBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        MockState.loaded_model = MODEL
        MockState.fail_stream = False
        MockState.fault = None
        MockState.last_chat_request = None
        self._home = tempfile.TemporaryDirectory()
        self.addCleanup(self._home.cleanup)
        self.env = dict(os.environ)
        self.env["USERPROFILE"] = self._home.name
        self.env["HOME"] = self._home.name
        self.env["XDG_CONFIG_HOME"] = self._home.name
        self.env.pop("STATE_DIRECTORY", None)

    def cli_command(self, *args):
        return [
            _config["cli_binary"] or get_default_cli_binary(),
            "--no-discovery",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.port),
            "chat",
            *args,
        ]

    def chat(self, *args, stdin=""):
        return subprocess.run(
            self.cli_command(*args),
            input=stdin,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
            env=self.env,
        )

    def session_files(self):
        return sorted(Path(self._home.name).rglob("chat_sessions/*.json"))

    def read_sessions(self):
        return [json.loads(p.read_text(encoding="utf-8")) for p in self.session_files()]

    def sent_roles(self):
        return [m["role"] for m in MockState.last_chat_request["messages"]]


class ChatSessionTests(ChatSessionBase):
    def test_one_shot_without_session_flags_saves_nothing(self):
        r = self.chat(MODEL, "-p", "hello")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.session_files(), [])

    def test_named_session_carries_history_across_calls(self):
        r = self.chat(MODEL, "--session", "notes", "-p", "first")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "Hello there\n")
        self.assertEqual(self.sent_roles(), ["user"])

        r = self.chat("--session", "notes", "-p", "second")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.sent_roles(), ["user", "assistant", "user"])
        self.assertEqual(MockState.last_chat_request["model"], MODEL)

        sessions = self.read_sessions()
        self.assertEqual(len(sessions), 1)
        saved = sessions[0]
        self.assertEqual(saved["version"], 1)
        self.assertEqual(saved["name"], "notes")
        self.assertEqual(saved["title"], "first")
        self.assertEqual(saved["model"], MODEL)
        self.assertEqual(
            [m["content"] for m in saved["messages"]],
            ["first", "Hello there", "second", "Hello there"],
        )
        self.assertEqual(saved["usage"]["turns"], 2)
        self.assertEqual(saved["usage"]["completion_tokens"], 4)
        self.assertEqual(self.session_files()[0].stem, saved["id"])

    def test_json_output_is_unchanged_by_sessions(self):
        r = self.chat(MODEL, "--session", "j", "-p", "hi", "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = json.loads(r.stdout)
        self.assertEqual(out["message"]["content"], "Hello there")
        self.assertEqual(len(self.read_sessions()), 1)

    def test_continue_picks_the_latest_session(self):
        self.chat(MODEL, "--session", "older", "-p", "one")
        self.chat(MODEL, "--session", "newer", "-p", "two")
        r = self.chat("--continue", "-p", "three")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(MockState.last_chat_request["messages"][0]["content"], "two")
        by_name = {s["name"]: s for s in self.read_sessions()}
        self.assertEqual(len(by_name["newer"]["messages"]), 4)
        self.assertEqual(len(by_name["older"]["messages"]), 2)

    def test_continue_without_sessions_fails(self):
        r = self.chat("--continue", "-p", "hi")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout, "")
        self.assertIn("no saved sessions", r.stderr)

    def test_resume_by_name_id_and_prefix(self):
        self.chat(MODEL, "--session", "work", "-p", "one")
        saved_id = self.read_sessions()[0]["id"]
        for target in ("work", saved_id, saved_id[:4]):
            r = self.chat("--resume", target, "-p", "again")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.sent_roles()[0], "user")
            self.assertGreater(len(MockState.last_chat_request["messages"]), 1)

    def test_resume_unknown_session_fails(self):
        r = self.chat(MODEL, "--resume", "missing", "-p", "hi")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout, "")
        self.assertIn("missing", r.stderr)
        self.assertIsNone(MockState.last_chat_request)

    def test_no_save_reads_history_but_writes_nothing(self):
        self.chat(MODEL, "--session", "ro", "-p", "one")
        before = self.session_files()[0].read_bytes()
        r = self.chat("--session", "ro", "--no-save", "-p", "two")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.sent_roles(), ["user", "assistant", "user"])
        self.assertEqual(self.session_files()[0].read_bytes(), before)

    def test_no_save_with_new_name_creates_nothing(self):
        r = self.chat(MODEL, "--session", "fresh", "--no-save", "-p", "one")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.session_files(), [])

    def test_system_prompt_is_stored_and_reused(self):
        self.chat(MODEL, "--session", "s", "--system", "be brief", "-p", "one")
        self.assertEqual(self.read_sessions()[0]["system_prompt"], "be brief")

        self.chat("--session", "s", "-p", "two")
        self.assertEqual(
            MockState.last_chat_request["messages"][0],
            {"role": "system", "content": "be brief"},
        )
        self.assertEqual(self.sent_roles(), ["system", "user", "assistant", "user"])

        self.chat("--session", "s", "--system", "be formal", "-p", "three")
        self.assertEqual(
            MockState.last_chat_request["messages"][0]["content"], "be formal"
        )
        self.assertEqual(self.read_sessions()[0]["system_prompt"], "be formal")

    def test_explicit_model_overrides_the_stored_one(self):
        self.chat(MODEL, "--session", "m", "-p", "one")
        r = self.chat("Other-Model", "--session", "m", "-p", "two")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(MockState.last_chat_request["model"], "Other-Model")
        self.assertEqual(self.read_sessions()[0]["model"], "Other-Model")

    def test_failed_request_is_not_saved(self):
        self.chat(MODEL, "--session", "f", "-p", "one")
        MockState.fail_stream = True
        r = self.chat("--session", "f", "-p", "two")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(len(self.read_sessions()[0]["messages"]), 2)

    def test_conflicting_selectors_are_usage_errors(self):
        for args in (
            ("--continue", "--resume", "x"),
            ("--continue", "--session", "x"),
            ("--resume", "x", "--session", "y"),
        ):
            r = self.chat(MODEL, *args, "-p", "hi")
            self.assertEqual(r.returncode, 2, args)
            self.assertEqual(r.stdout, "")

    def test_blank_session_values_are_usage_errors(self):
        for flag in ("--session", "--resume"):
            r = self.chat(MODEL, flag, " ", "-p", "hi")
            self.assertEqual(r.returncode, 2, flag)

    def test_corrupt_files_do_not_break_other_sessions(self):
        self.chat(MODEL, "--session", "good", "-p", "one")
        folder = self.session_files()[0].parent
        (folder / "deadbeef.json").write_text("{broken", encoding="utf-8")
        (folder / "abcd1234.9f9f.tmp").write_text("{partial", encoding="utf-8")
        r = self.chat("--continue", "-p", "two")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(MockState.last_chat_request["messages"]), 3)

    def test_session_from_newer_format_is_refused(self):
        self.chat(MODEL, "--session", "future", "-p", "one")
        path = self.session_files()[0]
        data = json.loads(path.read_text(encoding="utf-8"))
        data["version"] = 99
        path.write_text(json.dumps(data), encoding="utf-8")
        MockState.last_chat_request = None
        r = self.chat("--resume", "future", "-p", "two")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout, "")
        self.assertIsNone(MockState.last_chat_request)

    @unittest.skipIf(fcntl is None, "needs POSIX file locks")
    def test_one_shot_waits_for_a_session_open_elsewhere(self):
        self.chat(MODEL, "--session", "busy", "-p", "one")
        lock_path = self.session_files()[0].with_suffix(".lock")
        with open(lock_path, "a+", encoding="utf-8") as holder:
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            release = threading.Timer(1.0, lambda: fcntl.flock(holder, fcntl.LOCK_UN))
            release.start()
            r = self.chat("--session", "busy", "-p", "two")
            release.join()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.read_sessions()[0]["messages"]), 4)


@unittest.skipIf(pty is None, "needs a pseudo-terminal")
class ChatReplSessionTests(ChatSessionBase):
    def repl(self, *args, lines):
        master, slave = pty.openpty()
        try:
            proc = subprocess.Popen(
                self.cli_command(*args),
                stdin=slave,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                env=self.env,
                cwd=self._home.name,
            )
        finally:
            os.close(slave)
        try:
            os.write(master, "".join(f"{line}\n" for line in lines).encode())
            out, err = proc.communicate(timeout=60)
        finally:
            os.close(master)
        return proc.returncode, out, err

    def test_autosave_then_continue(self):
        rc, out, err = self.repl(MODEL, lines=["first question", "/exit"])
        self.assertEqual(rc, 0, err)
        self.assertIn("New session", out)
        self.assertIn("Hello there", out)
        sessions = self.read_sessions()
        self.assertEqual(len(sessions), 1)
        self.assertEqual(sessions[0]["title"], "first question")
        self.assertEqual(len(sessions[0]["messages"]), 2)
        self.assertIn("lemonade chat --resume " + sessions[0]["id"], out)

        rc, out, err = self.repl("--continue", lines=["second", "/exit"])
        self.assertEqual(rc, 0, err)
        self.assertIn("Resumed", out)
        self.assertEqual(self.sent_roles(), ["user", "assistant", "user"])
        self.assertEqual(len(self.read_sessions()[0]["messages"]), 4)

    def test_no_save_and_empty_sessions_write_nothing(self):
        self.repl(MODEL, "--no-save", lines=["hello", "/exit"])
        self.repl(MODEL, lines=["/exit"])
        self.assertEqual(self.session_files(), [])

    def test_rename_history_export_and_import(self):
        rc, out, err = self.repl(
            MODEL,
            lines=[
                "q1",
                "/rename alpha",
                "/export alpha.md",
                "/export alpha.json",
                "/export alpha.md",
                "/new beta",
                "q2",
                "/history",
                "2",
                "q3",
                "/exit",
            ],
        )
        self.assertEqual(rc, 0, err)
        self.assertIn("already exists", err)
        self.assertEqual(self.sent_roles(), ["user", "assistant", "user"])
        self.assertEqual(MockState.last_chat_request["messages"][0]["content"], "q1")
        by_name = {s["name"]: s for s in self.read_sessions()}
        self.assertEqual(len(by_name["alpha"]["messages"]), 4)
        self.assertEqual(len(by_name["beta"]["messages"]), 2)

        home = Path(self._home.name)
        exported = json.loads((home / "alpha.json").read_text(encoding="utf-8"))
        self.assertEqual([m["content"] for m in exported["messages"]][0], "q1")
        self.assertIn(
            "lemonade:message role=user", (home / "alpha.md").read_text("utf-8")
        )

        for name in ("alpha.md", "alpha.json"):
            rc, out, err = self.repl(MODEL, lines=[f"/import {name}", "q4", "/exit"])
            self.assertEqual(rc, 0, err)
            self.assertEqual(
                [m["content"] for m in MockState.last_chat_request["messages"]][0::2],
                ["q1", "q4"],
            )
        names = {s["name"] for s in self.read_sessions()}
        self.assertEqual(names, {"alpha", "beta", "alpha-2", "alpha-3"})

    def test_delete_asks_first(self):
        rc, out, err = self.repl(
            MODEL,
            lines=[
                "q1",
                "/rename keep",
                "/new",
                "q2",
                "/delete keep",
                "n",
                "/delete keep",
                "y",
                "/exit",
            ],
        )
        self.assertEqual(rc, 0, err)
        self.assertIn("cancelled", out)
        remaining = self.read_sessions()
        self.assertEqual(len(remaining), 1)
        self.assertEqual(remaining[0]["title"], "q2")

    def test_delete_accepts_a_row_from_history(self):
        rc, out, err = self.repl(
            MODEL,
            lines=["old", "/new", "newer", "/history", "", "/delete 2", "y", "/exit"],
        )
        self.assertEqual(rc, 0, err)
        remaining = self.read_sessions()
        self.assertEqual([s["title"] for s in remaining], ["newer"])

        rc, out, err = self.repl(MODEL, lines=["/delete 7", "/exit"])
        self.assertEqual(rc, 0, err)
        self.assertIn("no session", err)
        self.assertEqual(len(self.read_sessions()), 1)

    def test_commands_with_leading_spaces_are_not_messages(self):
        rc, out, err = self.repl(
            MODEL, lines=["q1", "  /session", "\t/history", "", "/exit"]
        )
        self.assertEqual(rc, 0, err)
        self.assertEqual(len(self.read_sessions()[0]["messages"]), 2)

    def test_importing_an_unnamed_export_is_distinguishable(self):
        self.repl(MODEL, lines=["first", "/export a.md", "/exit"])
        with open(Path(self._home.name) / "a.md", "a", encoding="utf-8") as exported:
            exported.write("\nadded by hand\n")
        rc, out, err = self.repl(MODEL, lines=["/import a.md", "/exit"])
        self.assertEqual(rc, 0, err)
        titles = sorted(s["title"] for s in self.read_sessions())
        self.assertEqual(titles, ["first", "first (imported)"])
        imported = next(s for s in self.read_sessions() if s["title"].endswith(")"))
        self.assertTrue(imported["messages"][-1]["content"].endswith("added by hand"))

    def test_deleting_the_current_session_starts_a_new_one(self):
        rc, out, err = self.repl(
            MODEL, lines=["q1", "/rename cur", "/delete cur", "y", "q2", "/exit"]
        )
        self.assertEqual(rc, 0, err)
        remaining = self.read_sessions()
        self.assertEqual(len(remaining), 1)
        self.assertEqual(remaining[0]["name"], "")
        self.assertEqual([m["content"] for m in remaining[0]["messages"]][0], "q2")

    def test_system_prompt_edit_keeps_history_but_replace_clears_it(self):
        rc, out, err = self.repl(
            MODEL,
            lines=["/system be formal", "q1", "/system --keep be brief", "q2", "/exit"],
        )
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.sent_roles(), ["system", "user", "assistant", "user"])
        self.assertEqual(
            MockState.last_chat_request["messages"][0]["content"], "be brief"
        )
        self.assertEqual(self.read_sessions()[0]["system_prompt"], "be brief")

        rc, out, err = self.repl(MODEL, lines=["q1", "/system fresh", "q2", "/exit"])
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.sent_roles(), ["system", "user"])

    def test_session_command_describes_the_session(self):
        rc, out, err = self.repl(MODEL, lines=["q1", "/session", "/exit"])
        self.assertEqual(rc, 0, err)
        saved = self.read_sessions()[0]
        self.assertIn(saved["id"], out)
        self.assertIn(".json", out)

    @unittest.skipIf(fcntl is None, "needs POSIX file locks")
    def test_session_open_elsewhere_is_refused(self):
        self.repl(MODEL, "--session", "busy", lines=["q1", "/exit"])
        lock_path = self.session_files()[0].with_suffix(".lock")
        with open(lock_path, "a+", encoding="utf-8") as holder:
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            rc, out, err = self.repl("--resume", "busy", lines=["q2", "/exit"])
            self.assertNotEqual(rc, 0)
            self.assertIn("open in another", err)

            rc, out, err = self.repl("--resume", "busy", "--no-save", lines=["/exit"])
            self.assertEqual(rc, 0, err)
        self.assertEqual(len(self.read_sessions()[0]["messages"]), 2)


class PtyRepl:
    """Drives lemonade chat on a real terminal (stdin and stdout are a pty)."""

    PROMPT_MARK = "Stop response or exit"

    def __init__(self, cmd, env, cwd):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(
            cmd,
            stdin=slave,
            stdout=slave,
            stderr=subprocess.PIPE,
            env=env,
            cwd=cwd,
        )
        os.close(slave)
        self.output = ""
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

    def _drain(self):
        while True:
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return
            if not data:
                return
            self.output += data.decode("utf-8", "replace")

    def wait_for_prompt(self, number):
        deadline = time.time() + 20
        while self.output.count(self.PROMPT_MARK) < number:
            if time.time() > deadline:
                raise AssertionError(
                    "prompt %d never appeared:\n%s" % (number, self.output)
                )
            time.sleep(0.02)
        time.sleep(0.2)

    def wait_for_text(self, text, since=0, timeout=20):
        deadline = time.time() + timeout
        while text not in self.output[since:]:
            if time.time() > deadline:
                raise AssertionError(
                    "%r never appeared:\n%s" % (text, self.output[since:])
                )
            time.sleep(0.02)
        time.sleep(0.2)

    def send(self, keys):
        os.write(self.master, keys.encode("utf-8"))

    def finish(self):
        try:
            self.proc.wait(timeout=20)
        finally:
            self._reader.join(timeout=5)
            os.close(self.master)
        stderr = self.proc.stderr.read().decode("utf-8", "replace")
        self.proc.stderr.close()
        return self.proc.returncode, stderr


@unittest.skipIf(pty is None, "needs a pseudo-terminal")
class ChatLineEditorTests(ChatSessionBase):
    def start(self, *args):
        return PtyRepl(self.cli_command(*args), self.env, self._home.name)

    def last_user_text(self):
        return MockState.last_chat_request["messages"][-1]["content"]

    def type_line(self, repl, prompt_number, keys, expect):
        repl.wait_for_prompt(prompt_number)
        repl.send(keys)
        repl.wait_for_prompt(prompt_number + 1)
        self.assertEqual(self.last_user_text(), expect)

    def test_editing_keys_and_history(self):
        repl = self.start(MODEL)
        self.type_line(repl, 1, "first\r", "first")
        self.type_line(repl, 2, "\x1b[A\r", "first")
        self.type_line(repl, 3, "abc\x1b[D\x1b[DX\r", "aXbc")
        self.type_line(repl, 4, "zzz\x15real\r", "real")
        self.type_line(repl, 5, "abc\x7f\r", "ab")
        self.type_line(repl, 6, "\x1b[A\x1b[A\x1b[B\r", "ab")
        self.type_line(repl, 7, "caf\xe9 \u2615\x1b[D\x1b[DX\r", "caf\xe9X \u2615")
        self.type_line(repl, 8, "one two\x17three\r", "one three")
        repl.wait_for_prompt(9)
        repl.send("\x03")
        rc, err = repl.finish()
        self.assertEqual(rc, 0, err)

    def test_escape_clears_the_line(self):
        repl = self.start(MODEL)
        repl.wait_for_prompt(1)
        repl.send("discard me\x1b")
        time.sleep(0.3)
        repl.send("kept\r")
        repl.wait_for_prompt(2)
        self.assertEqual(self.last_user_text(), "kept")
        repl.send("/exit\r")
        rc, err = repl.finish()
        self.assertEqual(rc, 0, err)

    def test_resumed_session_messages_are_in_the_history(self):
        self.chat(MODEL, "--session", "h", "-p", "from an earlier run")
        repl = self.start("--resume", "h")
        self.type_line(repl, 1, "\x1b[A\r", "from an earlier run")
        repl.send("/exit\r")
        rc, err = repl.finish()
        self.assertEqual(rc, 0, err)


@unittest.skipIf(pty is None, "needs a pseudo-terminal")
class ChatHistoryPickerTests(ChatSessionBase):
    def setUp(self):
        super().setUp()
        self.chat(MODEL, "--session", "alpha", "-p", "alpha question")
        self.chat(MODEL, "--session", "beta", "-p", "beta question")
        self.repl = PtyRepl(self.cli_command(MODEL), self.env, self._home.name)
        self.addCleanup(self.close)
        self.repl.wait_for_prompt(1)

    def close(self):
        if self.repl.proc.poll() is None:
            self.repl.send("\x03")
        self.repl.finish()

    def open_picker(self, command="/history"):
        mark = len(self.repl.output)
        self.repl.send(command + "\r")
        self.repl.wait_for_text("first ", mark)
        return len(self.repl.output)

    def first_sent_content(self):
        return MockState.last_chat_request["messages"][0]["content"]

    def say_and_check(self, prompt_number, expect):
        self.repl.send("x\r")
        self.repl.wait_for_prompt(prompt_number)
        self.assertEqual(self.first_sent_content(), expect)

    def test_arrows_and_enter_open_a_session(self):
        self.open_picker()
        self.repl.send("\x1b[B")
        time.sleep(0.2)
        self.repl.send("\r")
        self.repl.wait_for_prompt(2)
        self.say_and_check(3, "alpha question")

    def test_enter_opens_the_highlighted_newest_session(self):
        self.open_picker()
        self.repl.send("\r")
        self.repl.wait_for_prompt(2)
        self.say_and_check(3, "beta question")

    def test_slash_filters_the_list(self):
        self.open_picker()
        self.repl.send("/alp")
        time.sleep(0.3)
        self.assertIn("1 of 2", self.repl.output)
        self.repl.send("\r\r")
        self.repl.wait_for_prompt(2)
        self.say_and_check(3, "alpha question")

    def test_history_argument_searches_message_text(self):
        self.chat(MODEL, "--session", "gamma", "-p", "a needle in the haystack")
        self.open_picker("/history needle")
        self.repl.send("\r")
        self.repl.wait_for_prompt(2)
        self.say_and_check(3, "a needle in the haystack")

    def test_d_deletes_after_confirmation(self):
        self.open_picker()
        self.repl.send("dn")
        time.sleep(0.3)
        self.assertEqual(len(self.session_files()), 2)
        mark = len(self.repl.output)
        self.repl.send("dy")
        self.repl.wait_for_text("deleted", mark)
        self.repl.send("\x1b")
        self.repl.wait_for_prompt(2)
        self.assertEqual([s["name"] for s in self.read_sessions()], ["alpha"])

    def test_r_renames_a_session(self):
        self.open_picker()
        mark = len(self.repl.output)
        self.repl.send("r")
        self.repl.wait_for_text("Rename to", mark)
        mark = len(self.repl.output)
        self.repl.send("renamed\r")
        self.repl.wait_for_text("session named 'renamed'", mark)
        self.repl.send("\x1b")
        self.repl.wait_for_prompt(2)
        self.assertEqual(
            sorted(s["name"] for s in self.read_sessions()), ["alpha", "renamed"]
        )

    def test_escape_cancels_without_changing_anything(self):
        self.open_picker()
        self.repl.send("\x1b")
        self.repl.wait_for_prompt(2)
        self.say_and_check(3, "x")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli-binary", default=None)
    args, remaining = parser.parse_known_args()
    _config["cli_binary"] = args.cli_binary
    unittest.main(argv=[sys.argv[0]] + remaining)
