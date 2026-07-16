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
import io
import os
import re
import subprocess
import sys
import tokenize
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
TEST_PATH_RE = re.compile(r"(^|/)tests?(/|_|\.)|(^|/)testing(/|_)|_tests?\.|(^|/)test_")

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
# A `<` opens a header-name token (where `//` and `/*` are literal, not comments) in a
# closed set of contexts: an include-family preprocessor directive (`#`, or its `%:`
# digraph, then include/include_next/import/embed), a `__has_include`/`__has_embed`
# expression, or a module `import`. Matched against the line up to the `<`.
_INCLUDE_RE = re.compile(
    r"^\s*(?:#|%:)\s*(?:include(?:_next)?|import|embed)\b"
    r"|__has_(?:include(?:_next)?|embed)\s*\(\s*$"
    r"|^\s*(?:export\s+)?import\s*$"
)
SOURCE_SUFFIXES = (
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".py",
    ".pyi",
)


def is_comment_line(path, text):
    pattern = COMMENT_RE_PY if path.endswith((".py", ".pyi")) else COMMENT_RE_CISH
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
    # Decode losslessly. `errors="replace"` collapses every invalid byte to one U+FFFD,
    # so `\xff` and `\xfe` inside a string compare equal -- a change certified as no
    # change. `surrogateescape` maps each invalid byte to a distinct surrogate, so the
    # difference survives. And we do NOT translate newlines (no text=True), because a raw
    # CR inside a string literal (an HTTP template, say) is content, and `\r\n`->`\n`
    # would hide a CRLF change as comments-only.
    p = subprocess.run(cmd, capture_output=True, check=False)
    if p.returncode != 0:
        if allow_missing:
            return None
        msg = p.stderr.decode("utf-8", "replace").strip()
        raise GitError(f"{' '.join(cmd)} failed ({p.returncode}): {msg}")
    return p.stdout.decode("utf-8", "surrogateescape")


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


def _scan_line(text, was_open):
    """Read one line and return (block still open after, any code outside comments).

    `has_code` is what makes `/* note */ x = 1;` a code line rather than prose: the
    prefix `/*` opens and closes on the same line, and the `x = 1;` after it is code.
    String and char literals are content (code); a raw string holds `"` and `/*` as
    ordinary content and is consumed whole, so the quote tracker does not leave it early.
    """
    i, open_, quote, has_code = 0, was_open, "", False
    while i < len(text):
        ch = text[i]
        if quote:
            has_code = True
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif open_:
            if text.startswith("*/", i):
                open_ = False
                i += 2
                continue
        elif (raw := _raw_string_at(text, i)) is not None:
            has_code = True
            i += len(raw)
            continue
        elif ch in ('"', "'"):
            has_code = True
            quote = ch
        elif text.startswith("//", i):
            break
        elif text.startswith("/*", i):
            open_ = True
            i += 2
            continue
        elif not ch.isspace():
            has_code = True
        i += 1
    return open_, has_code


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
        if not path.endswith((".py", ".pyi")):
            in_block, has_code = _scan_line(text, in_block)
            is_comment = not has_code

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
    """The changed files whose code differs, or [] if only comments and blank lines do.

    A hardened mechanical check, not a formal proof. It soundly guards against a change
    that smuggles executable code in past a "comments only" claim (Python compared as an
    AST + token stream; C/C++ through a scanner modelling comments, splices, string/raw/
    header-name literals). It is NOT sound for directive SCOPE: a toolchain directive
    (`# fmt: off`, `# type: ignore`) moved between two identical code lines governs
    different code while the executable code is byte-identical, and that move is not
    detected. Reporting it soundly would require modelling every directive's governed
    region, which is out of scope; a moved linter/type directive surfaces in CI anyway.
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
    r"#\s*(type:\s*\S|noqa|nosec|bandit:|pylint:|pyright:|mypy:|ruff:|flake8:|isort:"
    r"|fmt:\s*(on|off|skip)|pragma:|sourcery\b"
    # A directive is generally `# <tool>: <verb>`. Match the VERB structurally rather than
    # enumerate every tool (yapf, autopep8, cspell, dlint, docformatter, ...) -- the tool
    # set is open, but the verbs that toggle behaviour are small. This does NOT match prose
    # like `# Note: something`, since the value must be a toggle word.
    r"|[\w.-]+:\s*(disable|enable|on|off|skip|ignore)\b)"
    r"|//\s*(NOLINT|clang-format\s+(on|off)|IWYU)"
    r"|/\*\s*(NOLINT|clang-format\s+(on|off)|IWYU)",
    re.IGNORECASE,
)

# PEP 263 honours an encoding cookie only on the first two lines, so matching it anywhere
# turns a comment that merely DISCUSSES encoding into a directive and reports a genuine
# comment edit as an offender.
CODING_RE = re.compile(r"^[ \t\f]*#.*?coding[:=][ \t]*[-_.a-zA-Z0-9]+")


def _directives_of(text):
    """The comments the toolchain OBEYS.

    `# type: int` is read by a type checker, and a first-line `#!` shebang by the kernel
    when the file is executed directly -- `python3` -> `python3 -O` silently disables
    every assert. Both are `#` comments, absent from the tree and the token stream, so a
    change to one would otherwise be certified as no-code-change; compare them here.
    """

    def code_part(s):
        # The code on a line with its trailing comment dropped -- a naive cut is fine for
        # an anchor even if a `//`/`#` sits inside a string, since it is not the trusted
        # code comparison, only a positional fingerprint.
        for marker in ("//", "#"):
            k = s.find(marker)
            if k != -1:
                s = s[:k]
        return s.strip()

    lines = text.splitlines()
    out = []
    for i, line in enumerate(lines):
        if (
            DIRECTIVE_RE.search(line)
            or (i == 0 and line.startswith("#!"))
            or (i < 2 and CODING_RE.match(line))
        ):
            anchor = code_part(line)
            if not anchor:
                # A standalone directive (`# fmt: off`) governs the code that FOLLOWS it,
                # so anchor to the next code line; fall back upward only at end of file.
                order = list(range(i + 1, len(lines))) + list(range(i - 1, -1, -1))
                for j in order:
                    if code_part(lines[j]):
                        anchor = code_part(lines[j])
                        break
            out.append((anchor, line.strip()))
    return out


def _code_of(text, path=""):
    """The code of a file, with comments and blank lines removed.

    String literals are tracked, because a regex that strips `//` anywhere would eat
    the rest of the line from inside "https://..." -- and two lines that differ only
    AFTER such a URL would then compare equal. That is a false PASS in the one place
    it must never happen: it would let a change advertised as a comment cleanup
    smuggle code past --assert-comments-only.
    """
    if path.endswith((".py", ".pyi")):
        return _code_of_python(text)
    # C replaces trigraphs in phase 1, before line splicing and comment removal, so `??/`
    # is a backslash that can continue a // comment over the next line. C++17 removed
    # them, so only .c is treated as trigraph-live; a .h here is a C++ header, where
    # detrigraphing would instead flag a literal `??/` in a comment as a code change.
    if path.endswith(".c"):
        text = _detrigraph(text)
    return _code_of_cish(text)


TRIGRAPHS = {
    "=": "#",
    "/": "\\",
    "'": "^",
    "(": "[",
    ")": "]",
    "!": "|",
    "<": "{",
    ">": "}",
    "-": "~",
}
TRIGRAPH_RE = re.compile(r"\?\?([=/'()!<>-])")


def _detrigraph(text):
    return TRIGRAPH_RE.sub(lambda m: TRIGRAPHS[m.group(1)], text)


def _code_of_python(text):
    """The program as BOTH a tree and a token stream, because each is blind where the
    other sees.

    The tree alone loses a literal's written form -- 0x100000 and 1048576 dump the same
    -- so a rewrite would be certified as no change. The tokens alone lose the block
    structure -- INDENT and DEDENT dropped, `bar()` inside an `if` and after it compare
    equal -- so moving a statement between scopes would be certified as no change.
    Requiring both to match means an edit has to survive two representations that fail
    in different directions.
    """
    try:
        tree = ast.dump(ast.parse(text))
        toks = [
            t.string
            for t in tokenize.generate_tokens(io.StringIO(text).readline)
            if t.type not in (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE)
        ]
    except (SyntaxError, ValueError, tokenize.TokenError, IndentationError) as e:
        # Refuse to certify what we could not parse.
        raise GitError(f"cannot parse Python source: {e}") from e
    return [tree, *toks]


def _after_splices(text, i):
    """Index of the first character at or after i that survives phase-2 line splicing."""
    while text[i : i + 2] == "\\\n":
        i += 2
    return i


def _header_name_at(text, i):
    """The header-name token at `<` (index i), splice-normalised, and the raw end index.

    Phase 2 splices `\\<LF>` before the header name is formed, so a spliced multi-line
    `<a//b\\<LF>c.h>` is the single header `<a//bc.h>`. Returns (None, None) if a raw
    (unspliced) newline is hit first -- then it is not a header name.
    """
    out = ["<"]
    j = i + 1
    while j < len(text):
        if text[j] == "\\" and text[j + 1 : j + 2] == "\n":
            j += 2
            continue
        if text[j] == "\n":
            return None, None
        out.append(text[j])
        if text[j] == ">":
            return "".join(out), j + 1
        j += 1
    return None, None


def _code_of_cish(text):
    # Phase 1: normalise line endings to \n, as the compiler does before splicing and
    # comment removal. Without this the splice below only matches `\`+`\n`, so on a CRLF
    # file deleting the `\` from a `// warn \` (which is `\`+`\r\n`) fails to splice and
    # the revived code compares equal. A line-ending change carries no code meaning (a
    # raw newline inside a "..." literal is ill-formed C), so normalising hides nothing.
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    # A raw string's interior whitespace and newlines are string CONTENT, not code
    # formatting -- `R"(  x)"` and `R"(x)"` are different constants. The line-split and
    # strip at the end would erase that, so raw strings are held out as opaque blobs
    # behind a placeholder and restored verbatim. The sentinel is grown until it cannot
    # occur in the source, so a placeholder can never collide with real content.
    sent = "\x00"
    while sent in text:
        sent += "\x00"
    raws = []
    out = []
    i, n = 0, len(text)
    in_line = in_block = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        # Phase 2: a backslash-newline joins two lines, and it happens BEFORE comments are
        # removed in phase 3 -- so deleting the backslash from a `// warn \` promotes the
        # line below it from dead code to live. It is spliced HERE rather than in a pass
        # over the whole text because C++ exempts raw string literals ([lex.phases]/2),
        # and a global splice would edit their CONTENTS: two raw strings differing by a
        # backslash-newline would then compare equal. The loop never steps inside a raw
        # string, because _raw_string_at consumes each one whole.
        if ch == "\\" and nxt == "\n":
            i += 2
            continue

        if in_line:
            if ch == "\n":
                in_line = False
                out.append(ch)
        elif in_block:
            # Phase 2 splices EVERY `\<LF>` before comments are removed, so `*`, any run of
            # them, then `/` is a `*/` that closes the comment. Splice to a fixed point
            # rather than matching one: a lone `*\<LF>/` check misses `*\<LF>\<LF>/`, and the
            # comment then swallows the rest of the file, comparing two revisions equal.
            if ch == "*":
                j = _after_splices(text, i + 1)
                if text[j : j + 1] == "/":
                    # Phase 3 replaces a comment with ONE space, so it separates tokens:
                    # `int/**/x` is `int x` (two tokens), not `intx`. Emit that space.
                    in_block = False
                    out.append(" ")
                    i = j + 1
                    continue
            if ch == "\n":
                out.append(ch)
        elif ch == "/" and text[(j := _after_splices(text, i + 1)) : j + 1] in (
            "/",
            "*",
        ):
            # An opener is spliceable too (`/\<LF>/`, `/\<LF>*`), for the same reason.
            in_line = text[j] == "/"
            in_block = not in_line
            i = j
        else:
            raw = _raw_string_at(text, i)
            if raw:
                out.append(f"{sent}{len(raws)}{sent}")
                raws.append(raw)
                i += len(raw)
                continue
            # In a header-name context the angle-bracket content is a single token: `//`
            # and `/*` inside it are literal, not comments. Scanning it as code truncates
            # `<a//b.h>` at the `//`, so a change to the header past the `//` -- a real,
            # different include -- compares equal. Consume the header name whole (phase-2
            # splices removed, as the compiler forms the token AFTER splicing), and protect
            # it from the final strip/split as content.
            if ch == "<" and _INCLUDE_RE.search("".join(out).rsplit("\n", 1)[-1]):
                hdr, end = _header_name_at(text, i)
                if hdr is not None and end is not None:
                    out.append(f"{sent}{len(raws)}{sent}")
                    raws.append(hdr)
                    i = end
                    continue
            if ch == '"' or ch == "'":
                # Hold the literal out as an opaque blob so its interior whitespace and
                # content survive the code-path whitespace collapse.
                lit, end = _quoted_at(text, i)
                out.append(f"{sent}{len(raws)}{sent}")
                raws.append(lit)
                i = end
                continue
            out.append(ch)
        i += 1
    if in_block:
        # An unterminated `/*` is not valid C, and swallowing it would certify two
        # differing revisions as equal. Refuse, as the Python path refuses to parse.
        raise GitError("unterminated /* block comment in C/C++ source")
    # Split on \n and strip only spaces/tabs, NOT \r: a `\r` inside a string literal is
    # content (a CRLF HTTP template differs from an LF one), and splitlines()/strip()
    # would erase it, certifying a CRLF->LF change of string content as comments-only.
    lines = [
        c
        for ln in "".join(out).split("\n")
        if (c := re.sub(r"[ \t]+", " ", ln).strip())
    ]
    if not raws:
        return lines
    return [
        re.sub(f"{sent}([0-9]+){sent}", lambda m: raws[int(m.group(1))], ln)
        for ln in lines
    ]


def _quoted_at(text, i):
    """The "..." or '...' literal at i, and the index just past it.

    A literal is held out as one opaque token so its interior whitespace is preserved --
    `"a  b"` and `"a b"` are different constants, and the code path collapses whitespace.
    `\\<LF>` inside is a phase-2 splice (removed); any other `\\x` is an escape (kept); an
    unescaped newline is ill-formed and refused.
    """
    q = text[i]
    out = [q]
    j = i + 1
    while j < len(text):
        c = text[j]
        if c == "\\":
            if text[j + 1 : j + 2] == "\n":
                j += 2
                continue
            out.append(c)
            if j + 1 < len(text):
                out.append(text[j + 1])
                j += 2
            else:
                j += 1
            continue
        if c == "\n":
            raise GitError("newline inside a string or character literal")
        out.append(c)
        j += 1
        if c == q:
            return "".join(out), j
    # Reached EOF without a closing quote: ill-formed, like an unterminated block comment.
    raise GitError("unterminated string or character literal")


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
        print("No code change detected: only comments and blank lines differ.")
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
