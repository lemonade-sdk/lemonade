#!/usr/bin/env python3
"""Flag comment slop in a diff: repeated explanations, oversized blocks, density.

Runs as a pre-commit hook (staged diff) or in CI (base..head). It reads a diff
rather than whole files, so it never complains about comments a contributor did
not write.

It flags; it never rewrites. Choosing which of several duplicate explanations to
keep needs judgement about where a confused reader lands, and that is not
mechanisable.

Usage:
    python tools/check_comment_slop.py                    # staged diff (pre-commit)
    python tools/check_comment_slop.py --from-ref origin/main --to-ref HEAD
    python tools/check_comment_slop.py --assert-comments-only --from-ref A --to-ref B
"""

import argparse
import ast
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field

# A concept re-explained at N sites is the dominant failure: the model restates it at
# every site it touches, having no memory of the last one. Those restatements are
# reworded, not copy-pasted, so prose similarity misses them -- what they share is
# distinctive vocabulary.
SHARED_TERMS_THRESHOLD = 4
MIN_TERM_LEN = 5
MAX_BLOCK_LINES = 12
DENSITY_WARN_PCT = 20

# Stating a contract at the declaration and the reason at the implementation is the
# idiomatic pair, not slop. Three sites is where a concept starts being re-explained.
MIN_SITES = 3

# A comment that points at the canonical explanation is a cross-reference, not a
# repeat of it -- which is exactly the shape a cut should leave behind.
POINTER_RE = re.compile(r"\bsee\s+\S", re.IGNORECASE)

# A test comment naming the invariant under test is the spec, even when the source
# explains the same mechanism.
TEST_PATH_RE = re.compile(r"(^|/)tests?(/|_)|(^|/)testing(/|_)|_tests?\.|(^|/)test_")

STOPWORDS = {
    "about",
    "after",
    "again",
    "against",
    "already",
    "always",
    "another",
    "because",
    "before",
    "being",
    "below",
    "between",
    "cannot",
    "could",
    "doing",
    "during",
    "either",
    "every",
    "first",
    "found",
    "from",
    "given",
    "hence",
    "however",
    "instead",
    "into",
    "itself",
    "later",
    "least",
    "leave",
    "makes",
    "might",
    "must",
    "never",
    "other",
    "rather",
    "same",
    "should",
    "since",
    "still",
    "such",
    "than",
    "that",
    "their",
    "them",
    "then",
    "there",
    "these",
    "they",
    "this",
    "those",
    "through",
    "under",
    "until",
    "using",
    "value",
    "when",
    "where",
    "which",
    "while",
    "with",
    "would",
    "your",
}

# In C and C++ a leading `#` opens a preprocessor directive, not a comment, and a
# leading `*` is a block-comment continuation only when nothing follows it -- `*ptr`
# is a dereference. Classifying either as a comment counts an ordinary include block
# as prose, which is how an alphabetised #include added to three files gets reported
# as a repeated explanation.
COMMENT_RE_PY = re.compile(r"^\s*#")
COMMENT_RE_CISH = re.compile(r"^\s*(//|/\*|\*/|\*(?=\s|$))")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".py")


def is_comment_line(path, text):
    pattern = COMMENT_RE_PY if path.endswith(".py") else COMMENT_RE_CISH
    return bool(pattern.match(text))


@dataclass
class Block:
    path: str
    line: int
    lines: list = field(default_factory=list)

    @property
    def text(self):
        return " ".join(self.lines)

    @property
    def terms(self):
        """Distinctive vocabulary. Identifiers are split (has_enforce_eager,
        --enforce-eager and VLLMServer::load all contribute their parts) so the same
        concept is recognised however it is spelled at each site."""
        parts = re.split(r"[^A-Za-z]+", self.text)
        words = []
        for part in parts:
            words += re.findall(r"[A-Z]?[a-z]+|[A-Z]+(?![a-z])", part)
        return {
            w.lower()
            for w in words
            if len(w) >= MIN_TERM_LEN and w.lower() not in STOPWORDS
        }


class GitError(RuntimeError):
    pass


def run(cmd, allow_missing=False):
    """Run a git command, or raise.

    A failed command that returned "" would be indistinguishable from one that
    succeeded and found nothing, so the verification would report a clean result for a
    comparison that never ran. `allow_missing` covers the one legitimate failure: a
    path absent from one side of the comparison.
    """
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if p.returncode != 0:
        if allow_missing:
            return None
        raise GitError(f"{' '.join(cmd)} failed ({p.returncode}): {p.stderr.strip()}")
    return p.stdout


HUNK_RE = re.compile(r"^@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def added_lines(diff):
    """Yield (path, lineno, text, is_comment) for every added line.

    The @@ header declares how many lines the hunk body holds, and we consume exactly
    that many. Telling body from header by prefix instead cannot be made correct: an
    added `++iter;` arrives as `+++iter;` and a removed `-- x` as `--- x`.
    """
    path, lineno, old_left, new_left = None, 0, 0, 0
    for raw in diff.splitlines():
        # Every body line carries a +/-/space/\ prefix, so a bare @@ or `diff --git` can
        # only be a header. Resyncing on one bounds the damage of a hunk whose declared
        # counts do not match its body to that hunk, instead of letting the shortfall eat
        # the headers that follow and silently swallow the next hunk's added lines.
        if HUNK_RE.match(raw) or raw.startswith("diff --git "):
            old_left = new_left = 0

        if old_left <= 0 and new_left <= 0:
            if raw.startswith("+++ b/"):
                path = raw[6:]
            elif m := HUNK_RE.match(raw):
                old_left = int(m.group(1) or 1)
                lineno = int(m.group(2))
                new_left = int(m.group(3) or 1)
            continue

        if raw.startswith("\\"):  # "\ No newline at end of file"
            continue
        if raw.startswith("+"):
            body = raw[1:]
            if path:
                yield path, lineno, body, is_comment_line(path, body)
            lineno += 1
            new_left -= 1
        elif raw.startswith("-"):
            old_left -= 1
        else:  # context
            lineno += 1
            old_left -= 1
            new_left -= 1


def strip_comment_markers(text):
    return re.sub(r"^\s*(//+|#+|/\*+|\*+/?)\s?", "", text).strip()


def _block_open_after(text, was_open):
    """Whether a C block comment is still open once this line has been read."""
    i, open_ = 0, was_open
    while i < len(text):
        if open_:
            if text.startswith("*/", i):
                open_ = False
                i += 2
                continue
        else:
            if text.startswith("//", i):
                break
            if text.startswith("/*", i):
                open_ = True
                i += 2
                continue
        i += 1
    return open_


def collect(diff):
    """Group added comment lines into contiguous blocks; count added code lines."""
    blocks, current, code = [], None, 0
    in_block, expected = False, None
    for path, lineno, text, is_comment in added_lines(diff):
        if not path.endswith(SOURCE_SUFFIXES):
            continue

        # The body of a /* */ block need not start its lines with a star, so whether a
        # line is prose depends on the lines before it. Carrying that state is only sound
        # across contiguous added lines -- over a gap, the lines we cannot see may have
        # closed the comment.
        if expected != (path, lineno):
            in_block = False
        expected = (path, lineno + 1)
        if not path.endswith(".py"):
            if in_block:
                is_comment = True
            in_block = _block_open_after(text, in_block)

        if is_comment:
            stripped = strip_comment_markers(text)
            if (
                current
                and current.path == path
                and current.line + len(current.lines) == lineno
            ):
                current.lines.append(stripped)
            else:
                current = Block(path, lineno, [stripped])
                blocks.append(current)
        else:
            current = None
            if text.strip():
                code += 1
    return blocks, code


def find_duplicates(blocks):
    """Group blocks that explain the same concept at different sites.

    A block joins a group only if it shares enough vocabulary with what the WHOLE
    group already has in common, not merely with one member. Matching against a
    single member chains unrelated blocks together (A-B, B-C, so A and C group
    despite sharing nothing) and reports a group with no common terms at all.

    Copy-pasted prose needs no separate check: identical text shares all its terms.
    """
    candidates = [
        b
        for b in blocks
        if len(b.terms) >= SHARED_TERMS_THRESHOLD
        and not TEST_PATH_RE.search(b.path)
        and not POINTER_RE.search(b.text)
    ]

    groups = []  # [members, terms common to every member]
    for block in candidates:
        terms = block.terms
        for group in groups:
            shared = group[1] & terms
            if len(shared) >= SHARED_TERMS_THRESHOLD:
                group[0].append(block)
                group[1] = shared  # stays >= threshold, so it can never empty out
                break
        else:
            groups.append([[block], set(terms)])
    return [(g, sorted(terms)) for g, terms in groups if len(g) >= MIN_SITES]


def comments_only(from_ref, to_ref):
    """True when the two revisions differ ONLY in comments and blank lines.

    A change that claims to be a comment cleanup should prove it rather than ask a
    reviewer to take it on faith.
    """
    offenders = []
    for path, old_mode, new_mode, status in _changed_entries(from_ref, to_ref):
        # The mode is part of the tree, so `chmod +x` changes the change even though
        # every byte of content is identical. Only a modified, same-mode source file can
        # possibly be a comment edit; everything else is reported rather than reasoned
        # about (added, deleted, renamed, retyped, or any non-source path).
        if old_mode != new_mode:
            offenders.append(f"{path} (mode {old_mode} -> {new_mode})")
            continue
        if status[:1] != "M" or not path.endswith(SOURCE_SUFFIXES):
            offenders.append(path)
            continue

        before = run(["git", "show", f"{from_ref}:{path}"], allow_missing=True)
        after = run(["git", "show", f"{to_ref}:{path}"], allow_missing=True)
        if before is None or after is None:
            offenders.append(path)
        elif _directives_of(before) != _directives_of(after):
            offenders.append(path)
        elif _code_of(before, path) != _code_of(after, path):
            offenders.append(path)
    return offenders


def _changed_entries(from_ref, to_ref):
    """(path, old_mode, new_mode, status) per changed path.

    --raw carries the file modes, which --name-only does not; -z keeps a path holding a
    space or a quote in one piece.
    """
    fields = run(["git", "diff", "--raw", "-z", f"{from_ref}..{to_ref}"]).split("\0")
    entries, i = [], 0
    while i < len(fields):
        meta = fields[i]
        if not meta.startswith(":"):
            i += 1
            continue
        old_mode, new_mode, _, _, status = meta[1:].split()
        n = 2 if status[:1] in ("R", "C") else 1  # rename and copy carry src and dst
        paths = fields[i + 1 : i + 1 + n]
        if paths:
            entries.append((paths[-1], old_mode, new_mode, status))
        i += 1 + n
    return entries


# A comment the toolchain reads is not decoration: dropping `# type: ignore` changes what
# mypy accepts, and an encoding cookie changes how the file is decoded. Neither survives
# into the AST or the stripped code, so compare them separately rather than certify a
# change to one as "comments only".
DIRECTIVE_RE = re.compile(
    r"#\s*(type:\s*ignore|noqa|pylint:|mypy:|fmt:\s*(on|off)|pragma:)"
    r"|#.*coding[:=]"  # PEP 263 writes it as `# -*- coding: utf-8 -*-`
    r"|//\s*(NOLINT|clang-format\s+(on|off)|IWYU)"
    r"|/\*\s*(NOLINT|clang-format\s+(on|off))",
    re.IGNORECASE,
)


def _directives_of(text):
    return [line.strip() for line in text.splitlines() if DIRECTIVE_RE.search(line)]


def _code_of(text, path=""):
    """The code of a file, with comments and blank lines removed.

    String literals are tracked, because a regex that strips `//` anywhere would eat
    the rest of the line from inside "https://..." -- and two lines that differ only
    AFTER such a URL would then compare equal. That is a false PASS in the one place
    it must never happen: it would let a change advertised as a comment cleanup
    smuggle code past --assert-comments-only.
    """
    if path.endswith(".py"):
        return _code_of_python(text)
    return _code_of_cish(text)


def _code_of_python(text):
    """The parsed program, as a tree.

    A token stream cannot carry a security claim: dropping INDENT and DEDENT makes
    `bar()` inside an `if` and `bar()` after it compare equal, so a change that moves a
    statement between scopes passes as comments-only. The tree is what the interpreter
    runs, and comments are absent from it by construction.
    """
    try:
        return ast.dump(ast.parse(text))
    except (SyntaxError, ValueError) as e:
        # Refuse to certify what we could not parse.
        raise GitError(f"cannot parse Python source: {e}") from e


def _code_of_cish(text):
    # C removes comments in translation phase 3, AFTER a backslash-newline splices two
    # lines together in phase 2. Skipping the splice means a `// warn \` whose next line
    # is code reads that line as live -- so DELETING the backslash, a change entirely
    # inside a comment, silently promotes dead code to live code and still compares equal.
    text = text.replace("\\\n", "")
    out = []
    i, n = 0, len(text)
    in_str = in_chr = in_line = in_block = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if ch == "\n":
                in_line = False
                out.append(ch)
        elif in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                i += 1
            elif ch == "\n":
                out.append(ch)
        elif in_str or in_chr:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 1
            elif (in_str and ch == '"') or (in_chr and ch == "'"):
                in_str = in_chr = False
        elif ch == "/" and nxt == "/":
            in_line = True
            i += 1
        elif ch == "/" and nxt == "*":
            in_block = True
            i += 1
        else:
            raw = _raw_string_at(text, i)
            if raw:
                out.append(raw)
                i += len(raw)
                continue
            if ch == '"':
                in_str = True
            elif ch == "'":
                in_chr = True
            out.append(ch)
        i += 1
    return [ln.strip() for ln in "".join(out).splitlines() if ln.strip()]


def _raw_string_at(text, i):
    """The raw string literal starting at i, or None.

    R"delim( ... )delim" takes no escapes and may hold bare quotes, so the ordinary
    string scanner would leave it early and read its contents as code.
    """
    if text[i] != "R" or not text[i + 1 : i + 2] == '"':
        return None
    open_paren = text.find("(", i + 2)
    if open_paren == -1:
        return None
    delim = text[i + 2 : open_paren]
    if len(delim) > 16 or re.search(r'[\s()\\"]', delim):
        return None
    close = ")" + delim + '"'
    end = text.find(close, open_paren)
    if end == -1:
        return None
    return text[i : end + len(close)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--from-ref")
    p.add_argument("--to-ref", default="HEAD")
    p.add_argument(
        "--assert-comments-only",
        action="store_true",
        help="fail unless the change touches comments and blank lines only",
    )
    p.add_argument("filenames", nargs="*", help="ignored; pre-commit passes these")
    args = p.parse_args()

    if args.assert_comments_only:
        if not args.from_ref:
            p.error("--assert-comments-only requires --from-ref")
        try:
            offenders = comments_only(args.from_ref, args.to_ref)
        except GitError as e:
            # A check that could not run has not passed.
            print(f"Could not verify: {e}")
            return 1
        if offenders:
            print("This change is NOT comments-only. Code changed in:")
            for f in offenders:
                print(f"  {f}")
            return 1
        print("Verified: comments and blank lines only; no code changed.")
        return 0

    # pre-commit exports these when invoked with --from-ref/--to-ref, which is how CI
    # runs it; a plain `git commit` has neither and we read the staged diff instead.
    from_ref = args.from_ref or os.environ.get("PRE_COMMIT_FROM_REF")
    to_ref = os.environ.get("PRE_COMMIT_TO_REF") or args.to_ref

    if from_ref:
        diff = run(["git", "diff", "--unified=0", f"{from_ref}..{to_ref}"])
    else:
        diff = run(["git", "diff", "--cached", "--unified=0"])

    blocks, code = collect(diff)
    comment_lines = sum(len(b.lines) for b in blocks)
    problems = 0

    for group, shared in find_duplicates(blocks):
        problems += 1
        print(
            f"This concept is explained at {len(group)} sites ({', '.join(shared[:5])}):"
        )
        for b in group:
            print(f"    {b.path}:{b.line}: {b.text[:70]}")
        print(
            "    Explain it once, where the surprising thing happens; "
            "point at that site from the others."
        )

    for b in blocks:
        if len(b.lines) > MAX_BLOCK_LINES:
            problems += 1
            print(
                f"{b.path}:{b.line}: {len(b.lines)}-line comment block "
                f"(limit {MAX_BLOCK_LINES})."
            )
            print("    Design notes belong in the PR description, not the source.")

    if code and comment_lines:
        density = comment_lines * 100 // (comment_lines + code)
        if density > DENSITY_WARN_PCT:
            print(
                f"note: {comment_lines} added comment lines to {code} of code ({density}%)."
            )

    if problems:
        print(
            f"\n{problems} issue(s). See AGENTS.md: comment the non-obvious WHY, never the WHAT."
        )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
