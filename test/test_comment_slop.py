#!/usr/bin/env python3
"""Tests for tools/check_comment_slop.py."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import check_comment_slop as slop  # noqa: E402


def diff(*hunks):
    """Build a unified diff (-U0) from (path, start_line, lines) triples."""
    out = []
    for path, start, lines in hunks:
        out.append(f"--- a/{path}")
        out.append(f"+++ b/{path}")
        out.append(f"@@ -{start},0 +{start},{len(lines)} @@")
        out += [f"+{line}" for line in lines]
    return "\n".join(out)


class CollectTests(unittest.TestCase):
    def test_contiguous_comment_lines_form_one_block(self):
        blocks, code = slop.collect(
            diff(("a.cpp", 10, ["// one", "// two", "int x = 1;"]))
        )
        self.assertEqual(len(blocks), 1)
        self.assertEqual(blocks[0].lines, ["one", "two"])
        self.assertEqual(code, 1)

    def test_code_between_comments_splits_the_block(self):
        blocks, _ = slop.collect(diff(("a.cpp", 1, ["// one", "int x = 1;", "// two"])))
        self.assertEqual(len(blocks), 2)

    def test_non_source_files_are_ignored(self):
        blocks, code = slop.collect(diff(("README.md", 1, ["# heading", "text"])))
        self.assertEqual(blocks, [])
        self.assertEqual(code, 0)


class TermTests(unittest.TestCase):
    def test_identifiers_are_split_into_words(self):
        block = slop.Block("a.cpp", 1, ["has_enforce_eager and VLLMServer::load"])
        self.assertIn("enforce", block.terms)
        self.assertIn("server", block.terms)

    def test_short_words_and_stopwords_are_dropped(self):
        block = slop.Block("a.cpp", 1, ["the cat sat on that mat"])
        self.assertEqual(block.terms, set())


class DuplicateTests(unittest.TestCase):
    def _blocks(self, *texts, path="a.cpp"):
        return [slop.Block(path, i * 10, [t]) for i, t in enumerate(texts)]

    def test_reworded_explanations_at_three_sites_are_flagged(self):
        # The real failure: the same concept, differently worded at each site.
        blocks = self._blocks(
            "enforce eager is stripped from args because load re-emits it from policy",
            "enforce eager is a managed intent, not passthrough: load re-emits policy",
            "policy re-emits enforce eager, so args must strip the managed intent",
        )
        found = slop.find_duplicates(blocks)
        self.assertEqual(len(found), 1)
        self.assertEqual(len(found[0][0]), 3)

    def test_two_sites_are_not_flagged(self):
        # Contract at the declaration + reason at the implementation is idiomatic.
        blocks = self._blocks(
            "enforce eager is stripped from args because load re-emits from policy",
            "enforce eager stripped here since load re-emits it from the policy",
        )
        self.assertEqual(slop.find_duplicates(blocks), [])

    def test_unrelated_blocks_are_not_grouped(self):
        blocks = self._blocks(
            "enforce eager stripped because load re-emits from launch policy",
            "speculative config is structured json that cannot ride the args string",
            "gfx covers every cdna generation plus vega, all discrete hbm parts",
        )
        self.assertEqual(slop.find_duplicates(blocks), [])

    def test_a_pointer_comment_is_not_a_duplicate(self):
        # "see X" is a cross-reference, which is what a cut should leave behind.
        blocks = self._blocks(
            "enforce eager is stripped from args because load re-emits from policy",
            "enforce eager is a managed intent, not passthrough: load re-emits policy",
            "enforce eager is absent here; see resolve_vllm_args, which manages policy",
        )
        self.assertEqual(slop.find_duplicates(blocks), [])

    def test_test_files_may_restate_the_invariant(self):
        blocks = self._blocks(
            "enforce eager is stripped from args because load re-emits from policy",
            "enforce eager is a managed intent, not passthrough: load re-emits policy",
        ) + self._blocks(
            "enforce eager must survive the resolver: load re-emits it from policy",
            path="test/cpp/test_thing.cpp",
        )
        self.assertEqual(slop.find_duplicates(blocks), [])

    def test_unrelated_blocks_never_chain_together(self):
        # A joins B and B joins C must not put A and C in one group when A and C
        # share nothing; that reports a "duplicate" with no common terms at all.
        # The bridge has to clear the threshold against B (4 terms) while sharing
        # nothing with A, or the chain never forms and the test proves nothing.
        blocks = self._blocks(
            "apple banana cherry delta eagle",
            "apple banana cherry delta flamingo gorilla hyena iguana",
            "flamingo gorilla hyena iguana jackfruit kangaroo leopard mongoose",
        )
        self.assertEqual(
            blocks[0].terms & blocks[1].terms, {"apple", "banana", "cherry", "delta"}
        )
        self.assertEqual(
            blocks[1].terms & blocks[2].terms,
            {"flamingo", "gorilla", "hyena", "iguana"},
        )
        self.assertEqual(blocks[0].terms & blocks[2].terms, set())
        self.assertEqual(slop.find_duplicates(blocks), [])


class CommentClassificationTests(unittest.TestCase):
    def test_preprocessor_directives_are_code_not_comments(self):
        # An alphabetised #include block is the most routine C++ diff there is. Read
        # as prose it becomes a "repeated explanation" the moment three files add one.
        self.assertFalse(slop.is_comment_line("a.cpp", "#include <lemon/router.h>"))
        self.assertFalse(slop.is_comment_line("a.cpp", "#define LEMON_MAX 4"))

    def test_hash_is_still_a_comment_in_python(self):
        self.assertTrue(slop.is_comment_line("a.py", "# why this is not obvious"))

    def test_block_comment_delimiters_are_comments(self):
        self.assertTrue(slop.is_comment_line("a.cpp", " */"))
        self.assertTrue(slop.is_comment_line("a.cpp", " * continued"))

    def test_leading_star_dereference_is_code(self):
        self.assertFalse(slop.is_comment_line("a.cpp", "    *result = compute();"))

    def test_a_block_comment_body_need_not_start_its_lines_with_a_star(self):
        blocks, code = slop.collect(
            diff(
                (
                    "a.cpp",
                    1,
                    [
                        "/* This is a long comment",
                        "   that continues without stars",
                        "   on each line",
                        "*/",
                    ],
                )
            )
        )
        self.assertEqual(len(blocks), 1)
        self.assertEqual(len(blocks[0].lines), 4)
        self.assertEqual(code, 0)

    def test_a_block_comment_closing_hands_the_next_line_back_to_code(self):
        blocks, code = slop.collect(
            diff(("a.cpp", 1, ["/* why", "   because", "*/", "int x = 1;"]))
        )
        self.assertEqual(len(blocks), 1)
        self.assertEqual(code, 1)

    def test_a_slashslash_comment_mentioning_a_block_opener_does_not_open_one(self):
        blocks, code = slop.collect(
            diff(("a.cpp", 1, ["// beware of /* in here", "int x = 1;"]))
        )
        self.assertEqual(len(blocks), 1)
        self.assertEqual(code, 1)

    def test_routine_include_block_is_not_slop(self):
        includes = [
            "#include <lemon/backends/wrapped_server.h>",
            "#include <lemon/server/model_manager.h>",
            "#include <lemon/server/router_capabilities.h>",
            "#include <lemon/server/wrapped_registry.h>",
        ]
        blocks, code = slop.collect(
            diff(
                ("alpha.cpp", 1, includes),
                ("bravo.cpp", 1, includes),
                ("charlie.cpp", 1, includes),
            )
        )
        self.assertEqual(blocks, [])
        self.assertEqual(code, 12)
        self.assertEqual(slop.find_duplicates(blocks), [])


class AddedLinesTests(unittest.TestCase):
    def _added(self, body):
        head = "diff --git a/x.cpp b/x.cpp\n--- a/x.cpp\n+++ b/x.cpp\n"
        return [t for _, _, t, _ in slop.added_lines(head + body)]

    def test_added_line_starting_with_plus_plus_survives(self):
        # "++iter;" reaches the parser as "+++iter;" -- a "+++" prefix test drops it.
        self.assertEqual(
            self._added("@@ -1,0 +1,2 @@\n+++iter;\n+int y = 1;\n"),
            ["++iter;", "int y = 1;"],
        )

    def test_removed_line_starting_with_dashes_does_not_derail(self):
        self.assertEqual(
            self._added("@@ -1,1 +1,1 @@\n--- x;\n+int y = 1;\n"), ["int y = 1;"]
        )

    def test_no_newline_marker_is_skipped(self):
        self.assertEqual(
            self._added("@@ -1,0 +1,1 @@\n+int y = 1;\n\\ No newline at end of file\n"),
            ["int y = 1;"],
        )

    def test_a_hunk_whose_counts_undershoot_cannot_swallow_the_next(self):
        # A hunk declaring more lines than it holds leaves the counter positive, and the
        # following @@ would be eaten as a body line -- taking the hunk after it with it.
        self.assertEqual(
            self._added("@@ -1 +1 @@\n+foo\n@@ -3 +3 @@\n+bar\n@@ -5 +5 @@\n+baz\n"),
            ["foo", "bar", "baz"],
        )

    def test_multi_file_diff_attributes_lines_to_each_file(self):
        d = (
            "diff --git a/a.cpp b/a.cpp\n--- a/a.cpp\n+++ b/a.cpp\n"
            "@@ -1,0 +1,1 @@\n+int a = 1;\n"
            "diff --git a/b.cpp b/b.cpp\n--- a/b.cpp\n+++ b/b.cpp\n"
            "@@ -1,0 +1,1 @@\n+int b = 2;\n"
        )
        self.assertEqual(
            [(p, t) for p, _, t, _ in slop.added_lines(d)],
            [("a.cpp", "int a = 1;"), ("b.cpp", "int b = 2;")],
        )

    def test_an_empty_diff_yields_nothing(self):
        self.assertEqual(list(slop.added_lines("")), [])


class TestPathTests(unittest.TestCase):
    def test_test_files_are_recognised(self):
        for path in ("test/a.py", "tests/a.py", "src/unit_tests.py", "testing/a.py"):
            self.assertTrue(slop.TEST_PATH_RE.search(path), path)

    def test_a_word_merely_containing_test_is_not_a_test_file(self):
        self.assertFalse(slop.TEST_PATH_RE.search("src/latest_version.py"))


class CodeOfTests(unittest.TestCase):
    def test_comments_and_blanks_are_erased_but_code_survives(self):
        self.assertEqual(
            slop._code_of("int x = 1;  // set x\n\n// a comment\nint y = 2;"),
            ["int x = 1;", "int y = 2;"],
        )

    def test_rewording_a_comment_leaves_the_code_identical(self):
        before = "// long-winded explanation\nint x = 1;"
        after = "// terse why\nint x = 1;"
        self.assertEqual(slop._code_of(before), slop._code_of(after))

    def test_adding_code_is_visible(self):
        before = "int x = 1;"
        after = "if (offline()) { return; }\nint x = 1;"
        self.assertNotEqual(slop._code_of(before), slop._code_of(after))

    def test_code_hidden_after_a_url_is_still_visible(self):
        # A regex that strips "//" anywhere eats the rest of the line from inside a URL,
        # so a change after it compares equal -- letting a "comment cleanup" smuggle code
        # past --assert-comments-only. That is the one false pass that must never happen.
        before = 'auto url = "https://example.com/old";'
        after = 'auto url = "https://example.com/new"; system("rm -rf /");'
        self.assertNotEqual(
            slop._code_of(before, "a.cpp"), slop._code_of(after, "a.cpp")
        )

    def test_code_hidden_after_a_url_is_still_visible_in_python(self):
        before = 'url = "https://example.com/old"'
        after = 'url = "https://example.com/new"; os.system("rm -rf /")'
        self.assertNotEqual(slop._code_of(before, "a.py"), slop._code_of(after, "a.py"))

    def test_a_raw_string_holding_quotes_does_not_end_the_literal_early(self):
        # Leaving R"(...)" at its first inner quote reads the rest of the JSON as code,
        # so a genuine comment-only change to such a file reports as NOT comments-only.
        before = '// old\nauto j = R"({"error":"nope"})";'
        after = '// terse why\nauto j = R"({"error":"nope"})";'
        self.assertEqual(slop._code_of(before, "a.cpp"), slop._code_of(after, "a.cpp"))

    def test_code_smuggled_after_a_raw_string_is_still_visible(self):
        before = 'auto j = R"({"a":"b"})";'
        after = 'auto j = R"({"a":"b"})"; system("rm -rf /");'
        self.assertNotEqual(
            slop._code_of(before, "a.cpp"), slop._code_of(after, "a.cpp")
        )

    def test_a_double_slash_inside_a_char_literal_is_not_a_comment(self):
        code = slop._code_of("char c = '\"'; int x = 1;", "a.cpp")
        self.assertEqual(code, ["char c = '\"'; int x = 1;"])

    def test_an_unterminated_string_fails_closed(self):
        before = 'auto s = "unterminated;\nint x = 1;'
        after = 'auto s = "unterminated;\nint x = 2;'
        self.assertNotEqual(
            slop._code_of(before, "a.cpp"), slop._code_of(after, "a.cpp")
        )

    def test_an_unparseable_python_file_fails_closed(self):
        self.assertNotEqual(
            slop._code_of("def f(:\n  x = 1", "a.py"),
            slop._code_of("def f(:\n  x = 2", "a.py"),
        )


if __name__ == "__main__":
    unittest.main()
