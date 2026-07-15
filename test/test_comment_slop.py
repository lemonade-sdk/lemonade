#!/usr/bin/env python3
"""Tests for tools/check_comment_slop.py."""

import importlib
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

    def test_an_inline_block_comment_before_code_is_a_code_line(self):
        # `/* note */ x = 1;` starts with `/*` but the code after the close is not prose.
        for line in ("/* note */ int evil = 1;", "*/ int x = 1;"):
            blocks, code = slop.collect(diff(("a.cpp", 1, [line])))
            self.assertEqual(blocks, [], line)
            self.assertEqual(code, 1, line)

    def test_a_string_containing_a_block_opener_does_not_open_one(self):
        # Reading `"/*"` as an open comment hands the NEXT line of ordinary code to the
        # slop detector as prose.
        blocks, code = slop.collect(
            diff(("a.cpp", 1, ['char s[] = "/*";', "int x = 1;"]))
        )
        self.assertEqual(blocks, [])
        self.assertEqual(code, 2)

    def test_a_raw_string_containing_a_block_opener_does_not_open_one(self):
        # A raw string holds `"` and `/*` as content; the quote tracker would leave it
        # early at the inner `"` and read the `/*` as a comment opener.
        blocks, code = slop.collect(
            diff(("a.cpp", 1, ['R"({"/*":"b"})"', "int evil();"]))
        )
        self.assertEqual(blocks, [])
        self.assertEqual(code, 2)

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
        self.assertFalse(slop.TEST_PATH_RE.search("contest.py"))

    def test_a_root_level_test_file_is_recognised(self):
        for path in ("test.cpp", "tests.py", "test.py"):
            self.assertTrue(slop.TEST_PATH_RE.search(path), path)


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
        # A newline inside a string literal is ill-formed C (gcc: missing terminating ").
        # Refuse it, rather than absorb the newline and hope a later line-split happens to
        # expose the change -- which it does not when both sides normalise to the same text.
        with self.assertRaises(slop.GitError):
            slop._code_of('auto s = "unterminated;\nint x = 1;', "a.cpp")

    def test_a_raw_line_ending_change_inside_a_string_does_not_compare_equal(self):
        # `"a\r\nb"` and `"a\nb"` are different string constants, but phase-1 line-ending
        # normalisation collapses both to `"a\nb"` -- so absorbing the newline would certify
        # a real change as comments-only. Both are ill-formed C, so fail closed.
        with self.assertRaises(slop.GitError):
            slop._code_of('char *s = "a\r\nb";\nint x = 1;\n', "a.c")
        with self.assertRaises(slop.GitError):
            slop._code_of('char *s = "a\nb";\nint x = 1;\n', "a.c")

    def test_a_backslash_continued_string_is_legal_and_does_not_fail_closed(self):
        # `"a\<newline>b"` is a legal line splice inside a string; it must NOT raise.
        self.assertEqual(
            slop._code_of('char *s = "a\\\nb";\nint x = 1;\n', "a.c"),
            slop._code_of('char *s = "a\\\nb";\nint x = 1;\n', "a.c"),
        )

    def test_a_backslash_newline_inside_a_raw_string_is_content_not_a_splice(self):
        # C++ deletes a backslash-newline EXCEPT inside a raw string ([lex.phases]/2).
        # Splicing the whole file first edits the raw string's CONTENTS, so two literals
        # that genuinely differ compare equal -- a false pass introduced BY the fix for
        # the line-continuation hole below.
        before = '// old\nauto s = R"delim(\nhello \\\nworld\n)delim";\n'
        after = '// new\nauto s = R"delim(\nhello world\n)delim";\n'
        self.assertNotEqual(
            slop._code_of(before, "a.cpp"), slop._code_of(after, "a.cpp")
        )

    def test_a_line_continuation_in_an_ordinary_string_is_still_spliced(self):
        # The exemption is raw strings ONLY; an ordinary string still splices.
        self.assertEqual(
            slop._code_of('auto s = "ab\\\ncd";\n', "a.cpp"),
            slop._code_of('auto s = "abcd";\n', "a.cpp"),
        )

    def test_a_trigraph_continuing_a_c_comment_is_caught(self):
        # In C, `??/` is a backslash, so `// x ??/` continues over the next line and
        # buries live code. C++17 removed trigraphs, so only .c is affected.
        plain = "// comment\nint x = 1;\n"
        trigraph = "// comment ??/\nint x = 1;\n"
        self.assertNotEqual(slop._code_of(plain, "a.c"), slop._code_of(trigraph, "a.c"))

    def test_a_literal_trigraph_in_a_cpp_header_is_a_comment_change(self):
        # A .h here is a C++ header; `??/` is three ordinary characters, so adding it to
        # a comment is genuinely comment-only and must not be flagged.
        plain = "// comment\nint x = 1;\n"
        trigraph = "// comment ??/\nint x = 1;\n"
        self.assertEqual(slop._code_of(plain, "a.h"), slop._code_of(trigraph, "a.h"))

    def test_invalid_utf8_degrades_instead_of_crashing(self):
        # run() decodes with errors="replace", so an undecodable file becomes a
        # comparison rather than an uncaught UnicodeDecodeError.
        self.assertIsInstance(slop.run(["printf", "\\xff\\xfe"]), str)

    def test_two_different_invalid_utf8_bytes_are_distinguished(self):
        # git output is decoded with surrogateescape, so \xff and \xfe inside a string
        # map to distinct surrogates. errors="replace" collapsed both to U+FFFD and hid
        # the change.
        self.assertNotEqual(
            slop._code_of('char m = "\udcff";\n', "a.c"),
            slop._code_of('char m = "\udcfe";\n', "a.c"),
        )

    def test_a_raw_crlf_inside_a_python_triple_quote_is_a_code_change(self):
        # A raw CR inside a Python triple-quoted string is a real value difference, and
        # the Python path is not line-normalised. (The C case is ill-formed -- a raw
        # newline in a "..." literal -- so the C scanner normalises line endings instead.)
        self.assertNotEqual(
            slop._code_of('h = """OK\r\n\r\n"""\n', "a.py"),
            slop._code_of('h = """OK\n\n"""\n', "a.py"),
        )

    def test_a_star_backslash_newline_slash_closes_a_block_comment(self):
        # `*\<newline>/` splices to `*/`, closing the comment in the compiler; the `*` and
        # `/` are never adjacent, so the scanner must match the spliced form or it
        # swallows the code after it as prose.
        before = "int x = 0; /* old *\\\n/\nevil();\nint y = 1;\n"
        after = "int x = 0; /* new *\\\n/\ngood();\nint y = 1;\n"
        self.assertNotEqual(slop._code_of(before, "a.c"), slop._code_of(after, "a.c"))

    def test_a_line_continuation_in_a_crlf_file_revives_dead_code(self):
        # `\`+CRLF continues a // comment just as `\`+LF does, once line endings are
        # normalised. Deleting the backslash promotes the line below from dead to live.
        before = "// disable this \\\r\nevil();\r\nint x = 1;\r\n"
        after = "// disable this\r\nevil();\r\nint x = 1;\r\n"
        self.assertNotEqual(slop._code_of(before, "a.c"), slop._code_of(after, "a.c"))

    def test_a_cr_only_file_gets_line_boundaries(self):
        # An old-Mac CR-only file starting with `//` was read as one whole-file comment,
        # hiding every code change inside it. Normalising CR to newline restores the
        # boundaries so a change is seen.
        self.assertNotEqual(
            slop._code_of("// header\rint x = 1;\r", "a.c"),
            slop._code_of("// header\rint x = 2;\r", "a.c"),
        )

    def test_a_crlf_source_file_with_a_genuine_comment_edit_still_passes(self):
        # The fix must not flag an ordinary comment edit in a CRLF-saved file -- both
        # sides carry the same line-ending \r, so the code compares equal.
        self.assertEqual(
            slop._code_of("// a\r\nint x = 1;\r\n", "a.c"),
            slop._code_of("// b\r\nint x = 1;\r\n", "a.c"),
        )

    def test_a_block_comment_closed_by_repeated_splices_does_not_swallow_the_file(self):
        # Phase 2 splices EVERY backslash-newline, so `*\<LF>\<LF>/` is a `*/` and the code
        # after it is LIVE (gcc -E agrees). Matching a single splice missed this and the
        # comment ate the rest of the file, comparing the two revisions equal.
        for pad in ("\\\n", "\\\n\\\n", "\\\n\\\n\\\n"):
            self.assertNotEqual(
                slop._code_of(f"/* *{pad}/ int x = old();\n", "a.c"),
                slop._code_of(f"/* *{pad}/ int x = new();\n", "a.c"),
                pad,
            )

    def test_a_comment_opener_split_by_a_splice_still_opens_a_comment(self):
        # `/\<LF>/` splices to `//`, so the text after it is a comment, not code.
        self.assertEqual(
            slop._code_of("/\\\n/ old\nint x = 1;\n", "a.c"),
            slop._code_of("/\\\n/ new\nint x = 1;\n", "a.c"),
        )

    def test_whitespace_inside_a_raw_string_is_content_not_formatting(self):
        # `R"(  x)"` and `R"(x)"` are different string constants (gcc keeps the spaces),
        # so the final line-strip must not erase interior raw-string whitespace.
        self.assertNotEqual(
            slop._code_of('auto s = R"(\n  hello\n)";\nint x = 1;\n', "a.cpp"),
            slop._code_of('auto s = R"(\nhello\n)";\nint x = 1;\n', "a.cpp"),
        )

    def test_a_blank_line_inside_a_raw_string_is_content(self):
        # A dropped blank line inside a raw string changes the constant.
        self.assertNotEqual(
            slop._code_of('auto s = R"(\nx\n\ny\n)";\n', "a.cpp"),
            slop._code_of('auto s = R"(\nx\ny\n)";\n', "a.cpp"),
        )

    def test_a_comment_edit_beside_an_unchanged_raw_string_still_passes(self):
        # The raw-string guard must not make a genuine comment-only change fail.
        self.assertEqual(
            slop._code_of('// old\nauto s = R"(\n  keep\n)";\n', "a.cpp"),
            slop._code_of('// new\nauto s = R"(\n  keep\n)";\n', "a.cpp"),
        )

    def test_a_slash_slash_inside_an_include_is_a_header_name_not_a_comment(self):
        # `<a//b.h>` is a header-name token; gcc keeps the `//` literal, so a change to the
        # header past the `//` is a real, different include -- not comments-only.
        self.assertNotEqual(
            slop._code_of(
                "#include <boost//algorithm//string.hpp>\nint x = 1;\n", "a.cpp"
            ),
            slop._code_of(
                "#include <boost//algorithm//vector.hpp>\nint x = 1;\n", "a.cpp"
            ),
        )

    def test_a_comment_edit_on_an_include_line_still_passes(self):
        # The header-name guard must not flag a genuine comment change after the include.
        self.assertEqual(
            slop._code_of("#include <vector>  // old\nint x = 1;\n", "a.cpp"),
            slop._code_of("#include <vector>  // new\nint x = 1;\n", "a.cpp"),
        )

    def test_an_ordinary_less_than_is_not_treated_as_a_header_name(self):
        # `<` outside an include directive is an operator: the change must be caught and a
        # trailing comment must still be stripped.
        self.assertNotEqual(
            slop._code_of("int r = a < b; // c\n", "a.cpp"),
            slop._code_of("int r = a > b; // c\n", "a.cpp"),
        )
        self.assertEqual(
            slop._code_of("int r = a < b; // old\n", "a.cpp"),
            slop._code_of("int r = a < b; // new\n", "a.cpp"),
        )

    def test_an_unterminated_block_comment_fails_closed(self):
        # Not valid C. Swallowing it certified two differing revisions as equal.
        with self.assertRaises(slop.GitError):
            slop._code_of("/* never closed\nint evil();\n", "a.c")

    def test_an_unparseable_python_file_fails_closed(self):
        # Refuse to certify what we could not parse, rather than compare two blanks.
        with self.assertRaises(slop.GitError):
            slop._code_of("def f(:\n  x = 1", "a.py")

    def test_moving_a_statement_into_a_block_is_not_a_comment_change(self):
        # Same tokens, different program: bar() runs unconditionally, then only when x.
        # A token stream that drops INDENT/DEDENT cannot see this.
        before = "def f(x):\n    if x:\n        foo()\n    bar()\n"
        after = "def f(x):\n    if x:\n        foo()\n        bar()\n"
        self.assertNotEqual(slop._code_of(before, "a.py"), slop._code_of(after, "a.py"))

    def test_rewording_a_comment_still_leaves_python_code_identical(self):
        before = "# long-winded explanation\ndef f(x):\n    return x\n"
        after = "# terse why\ndef f(x):\n    return x\n"
        self.assertEqual(slop._code_of(before, "a.py"), slop._code_of(after, "a.py"))

    def test_deleting_a_line_continuation_revives_dead_code(self):
        # The backslash continues the // comment onto the next line, so evil() is dead.
        # Removing it -- a change entirely inside a comment -- makes evil() live code.
        before = "// disable this \\\nevil();\nint x = 1;\n"
        after = "// disable this\nevil();\nint x = 1;\n"
        self.assertNotEqual(
            slop._code_of(before, "a.cpp"), slop._code_of(after, "a.cpp")
        )


class ChangedEntryTests(unittest.TestCase):
    def _entries(self, raw):
        slop.run = lambda *a, **k: raw  # noqa: ARG005
        try:
            return slop._changed_entries("A", "B")
        finally:
            importlib.reload(slop)

    def test_a_mode_change_is_reported(self):
        # chmod +x leaves every byte of content identical, so a content comparison sees
        # nothing -- but the mode is part of the tree, so the change is not comments-only.
        raw = ":100644 100755 b859599 b859599 M\0a.py\0"
        self.assertEqual(self._entries(raw), [("a.py", "100644", "100755", "M")])

    def test_a_path_containing_a_space_survives(self):
        raw = ":100644 100644 aaa bbb M\0my file.py\0"
        self.assertEqual(self._entries(raw)[0][0], "my file.py")

    def test_a_rename_reports_the_destination(self):
        raw = ":100644 100644 aaa bbb R100\0old.py\0new.py\0"
        self.assertEqual(self._entries(raw), [("new.py", "100644", "100644", "R100")])


class FileSelectionTests(unittest.TestCase):
    def test_a_filename_containing_a_space_is_one_path(self):
        # Splitting the file list on whitespace shatters `model.c manager.py` into two
        # paths that both exist and are unchanged, so the real file is never examined
        # and the change is certified as comments-only.
        raw = ":000000 100644 0000000 aaaaaaa A\0model.c manager.py\0"
        slop.run = lambda *a, **k: raw  # noqa: ARG005
        try:
            self.assertEqual(
                slop._changed_entries("A", "B")[0][0], "model.c manager.py"
            )
        finally:
            importlib.reload(slop)

    def test_python_stubs_are_parsed_as_python(self):
        # A .pyi does not end with ".py", so a suffix test that checks for it routes stub
        # files through the C scanner.
        self.assertEqual(
            slop._code_of("# old\ndef f() -> int: ...\n", "a.pyi"),
            slop._code_of("# new\ndef f() -> int: ...\n", "a.pyi"),
        )
        self.assertNotEqual(
            slop._code_of("def f() -> int: ...\n", "a.pyi"),
            slop._code_of("def f() -> str: ...\n", "a.pyi"),
        )


class TwoRepresentationTests(unittest.TestCase):
    """The tree and the token stream are each blind where the other sees."""

    def test_a_literal_rewritten_to_the_same_value_is_still_a_code_change(self):
        # ast.dump keeps the VALUE and loses the written form, so the tree alone calls
        # these equal. The token stream sees it.
        self.assertNotEqual(
            slop._code_of("B = 0x100000\n", "a.py"),
            slop._code_of("B = 1048576\n", "a.py"),
        )

    def test_implicit_string_concatenation_is_still_a_code_change(self):
        self.assertNotEqual(
            slop._code_of("s = 'a' 'b'\n", "a.py"), slop._code_of("s = 'ab'\n", "a.py")
        )

    def test_a_statement_moved_between_scopes_is_still_a_code_change(self):
        # The token stream loses INDENT/DEDENT, so tokens alone call these equal. The
        # tree sees it. Neither representation is sufficient by itself.
        self.assertNotEqual(
            slop._code_of("def f(x):\n    if x:\n        g()\n    h()\n", "a.py"),
            slop._code_of("def f(x):\n    if x:\n        g()\n        h()\n", "a.py"),
        )


class DirectiveTests(unittest.TestCase):
    def test_a_variable_type_comment_is_obeyed_by_the_toolchain(self):
        # `# type: int` is read by a type checker and appears in neither the tree nor
        # the token stream, so it can only be caught here.
        self.assertNotEqual(
            slop._directives_of("x = c()  # type: int"),
            slop._directives_of("x = c()  # type: str"),
        )

    def test_other_linter_directives_are_obeyed_too(self):
        for a, b in (
            ("import x  # pyright: ignore", "import x  # pyright: basic"),
            ("import x  # ruff: noqa", "import x"),
            ("import x  # isort: skip", "import x"),
            ("x = 1  # fmt: skip", "x = 1"),
            ("os.system(c)  # nosec", "os.system(c)"),
            ("/* IWYU pragma: keep */", "/* IWYU pragma: export */"),
        ):
            self.assertNotEqual(slop._directives_of(a), slop._directives_of(b), a)

    def test_a_shebang_is_obeyed_by_the_kernel(self):
        # A shebang is a `#` comment, but the kernel reads it to exec the file; python3
        # -> python3 -O silently disables every assert. So a shebang change is not a
        # comment-only change.
        self.assertNotEqual(
            slop._directives_of("#!/usr/bin/env python3\nx = 1"),
            slop._directives_of("#!/usr/bin/env python3 -O\nx = 1"),
        )

    def test_a_first_line_hash_that_is_not_a_shebang_is_not_a_directive(self):
        self.assertEqual(slop._directives_of("# just a comment\nx = 1"), [])

    def test_an_encoding_cookie_is_honoured_only_on_the_first_two_lines(self):
        # PEP 263 only reads the cookie there -- and a line matching its regex IS a
        # declaration, prose or not: CPython rejects `# the encoding: moved` with
        # "SyntaxError: encoding problem: moved". So matching it is correct, not
        # overconservative. Below line 2 it is inert and must not be treated as one.
        self.assertTrue(slop._directives_of("# -*- coding: utf-8 -*-\nx = 1"))
        self.assertFalse(slop._directives_of("x = 1\ny = 2\n# coding: utf-8 talk"))

    def test_a_directive_comment_is_not_decoration(self):
        # These read as comments but the toolchain obeys them, and they survive into
        # neither the AST nor the stripped code -- so compare them in their own right.
        for line in (
            "x = f()  # type: ignore",
            "import os  # noqa: F401",
            "# -*- coding: latin-1 -*-",
            "int x;  // NOLINT",
        ):
            self.assertTrue(slop._directives_of(line), line)

    def test_ordinary_comments_are_not_directives(self):
        self.assertEqual(
            slop._directives_of("# why this is not obvious\n// and here"), []
        )


if __name__ == "__main__":
    unittest.main()
