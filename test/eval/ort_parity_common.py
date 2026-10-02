"""
Shared pieces for the ort-server A/B parity scripts:

    ort_tokenizer_parity.py  full-corpus input_ids parity, old vs new tokenizer layer
    ort_server_parity.py     two ort-server binaries, same inputs, label-by-label diff
    ort_router_replay.py     lemond + candidate ort-server vs a recorded router run

Everything here mirrors a specific piece of C++ so the scripts send the model
exactly what production sends it. When the C++ changes, change the mirror.
"""

import json
import random
import sys
import unicodedata
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

DEFAULT_CORPUS = Path("test/conformance/routing/1/l2_pii_nemotron_20k")

# BIO / BIOES prefixes stripped when reporting detected entity types.
TAG_PREFIXES = ("B-", "I-", "E-", "S-", "L-", "U-")


def routing_input(request: dict) -> str:
    """The text lemond hands a classifier for a chat request: the content of the
    LAST user message (build_route_context, routing_classifier_services.cpp).

    Only string content is mirrored. Part-array content goes through
    collect_text_from_content(), whose joining rules are not reproduced here, so
    it is refused rather than approximated.
    """
    for msg in reversed(request.get("messages") or []):
        if isinstance(msg, dict) and msg.get("role") == "user" and "content" in msg:
            content = msg["content"]
            if not isinstance(content, str):
                raise ValueError("non-string message content is not mirrored")
            return content
    return ""


def load_cases(corpus_dir: Path) -> list[dict]:
    cases = []
    with open(corpus_dir / "cases.jsonl", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            case = json.loads(line)
            cases.append(
                {
                    "name": case["name"],
                    "text": routing_input(case["request"]),
                    "pii": case.get("pii_category", "none") != "none",
                }
            )
    return cases


def non_ascii_count(text: str) -> int:
    return sum(1 for ch in text if ord(ch) > 127)


def select_sample(
    cases: list[dict], n: int, longest: int, non_ascii: int, seed: int
) -> list[dict]:
    """Deterministic sample, tagged by why each case was picked.

    Long documents stress truncation, non-ASCII ones stress normalization and
    byte-level merges -- the two places a tokenizer upgrade can move ids -- and
    every non-PII case is always kept because the corpus has almost none.
    """
    if n <= 0 or n >= len(cases):
        return [dict(c, why="all") for c in cases]
    chosen: dict[str, dict] = {}

    def take(pool, k, why):
        for c in pool:
            if k <= 0:
                break
            if c["name"] not in chosen:
                chosen[c["name"]] = dict(c, why=why)
                k -= 1

    take([c for c in cases if not c["pii"]], len(cases), "benign")
    take(sorted(cases, key=lambda c: len(c["text"]), reverse=True), longest, "longest")
    by_non_ascii = sorted(cases, key=lambda c: non_ascii_count(c["text"]), reverse=True)
    take(
        [c for c in by_non_ascii if non_ascii_count(c["text"]) > 0],
        non_ascii,
        "non-ascii",
    )
    rest = [c for c in cases if c["name"] not in chosen]
    random.Random(seed).shuffle(rest)
    take(rest, n - len(chosen), "random")
    return list(chosen.values())


_ACCENTED = "José Müller lives at Straße 12, 80331 München; café crème, naïve Zoë."
_PII_SENTENCE = (
    "Please update the file for Margaret Okonkwo, SSN 518-23-4471, "
    "email m.okonkwo@example.com, phone (312) 555-0187."
)
_FILLER = (
    "The committee reviewed the quarterly maintenance schedule for the "
    "municipal water treatment facility and noted that the filtration "
    "upgrades remain on track for completion before the rainy season. "
)


def probe_texts() -> list[tuple[str, str]]:
    """Synthetic inputs aimed at the edges a tokenizer swap can move.

    The two long probes sit past pplx's 4,096-token tokenizer.json truncation,
    which the old tokenizer layer applied and tok_ffi deliberately clears:
    `long-pii-at-end` puts its only PII after that point, so the old build
    cannot see it and the new one can.
    """
    return [
        ("empty", ""),
        ("whitespace", " \n\t  "),
        ("single-char", "a"),
        ("ascii-pii", _PII_SENTENCE),
        (
            "emoji",
            "Ping me 🚀🔥 at jane.doe@example.org 👍🏽👨‍👩‍👧 or +1 415 555 0100 🙂",
        ),
        ("cjk", "我的名字是张伟，电话是13800138000，住在北京市朝阳区建国路88号。"),
        (
            "japanese",
            "山田太郎です。メールは taro.yamada@example.jp、電話は 090-1234-5678 です。",
        ),
        (
            "arabic-rtl",
            "اسمي محمد العلي ورقم هاتفي 0501234567 وبريدي mohammed@example.sa",
        ),
        ("devanagari", "मेरा नाम राहुल शर्मा है और मेरा फ़ोन नंबर 9876543210 है।"),
        ("accents-nfc", unicodedata.normalize("NFC", _ACCENTED)),
        ("accents-nfd", unicodedata.normalize("NFD", _ACCENTED)),
        ("zero-width", "Jo​hn Sm‍ith⁠, IBAN DE89 3704 0044 0532 0130 00"),
        ("bom-prefix", "﻿Patient: Maria Garcia, DOB 04/12/1987"),
        ("math-alnum", "𝒥𝑜𝒽𝓃 𝒮𝓂𝒾𝓉𝒽 𝟙𝟚𝟛-𝟜𝟝-𝟞𝟟𝟠𝟡"),
        ("fullwidth", "ＪＯＨＮ　ＳＭＩＴＨ　１２３－４５－６７８９"),
        (
            "special-token-literals",
            "[CLS] John [SEP] <s> Mary </s> <|endoftext|> <|startoftext|> [MASK] [UNK] <pad> [PAD]",
        ),
        ("control-chars", "Name:\x00Bob\x07Jones\x1b[0m card 4111 1111 1111 1111\r\n"),
        ("long-whitespace-run", "Alice" + " " * 4000 + "Baker 555-0100"),
        ("long-single-word", "x" * 20000),
        ("long-pii-throughout", (_FILLER + _PII_SENTENCE + " ") * 90),
        ("long-pii-at-end", _FILLER * 230 + _PII_SENTENCE),
    ]


def _read_json(path: Path) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            j = json.load(f)
        return j if isinstance(j, dict) else {}
    except (OSError, ValueError):
        return {}


def _budget(j: dict, key: str) -> int:
    # nlohmann parses HF's 1e30 "no limit" sentinel as a float and the C++
    # skips it via is_number_integer(); Python reads the same literal as an int,
    # so the range check is what keeps the two in agreement.
    v = j.get(key)
    if isinstance(v, bool) or not isinstance(v, int):
        return 0
    return v if 2 <= v <= 1_000_000 else 0


def ort_max_length(model_dir: Path) -> int:
    """ort-server's token budget for an encoder model (apply_inferred_max_length)."""
    manifest = _read_json(model_dir / "manifest.json")
    if isinstance(manifest.get("max_length"), int):
        return manifest["max_length"]
    if n := _budget(
        _read_json(model_dir / "tokenizer_config.json"), "model_max_length"
    ):
        return n
    if n := _budget(_read_json(model_dir / "config.json"), "max_position_embeddings"):
        return n - 2 if n > 4 else n
    return 512


def ort_pad_id(tokenizer_json: dict) -> int | None:
    padding = tokenizer_json.get("padding")
    if isinstance(padding, dict) and isinstance(padding.get("pad_id"), int):
        return padding["pad_id"]
    return None


def ort_encoder_input_ids(
    ids: list[int], max_len: int, pad_id: int | None
) -> list[int]:
    """Model.classify()'s post-tokenizer steps: strip trailing padding, then cut
    to the budget keeping the final token."""
    ids = list(ids)
    if pad_id is not None:
        while len(ids) > 1 and ids[-1] == pad_id:
            ids.pop()
    if len(ids) > max_len:
        ids = ids[: max_len - 1] + [ids[-1]]
    return ids


def parse_model_args(values: list[str]) -> list[tuple[str, Path]]:
    models = []
    for v in values:
        name, sep, path = v.partition("=")
        if not sep or not name or not path:
            raise SystemExit(f"--model expects NAME=DIR, got {v!r}")
        p = Path(path).expanduser()
        if not (p / "model.onnx").exists() or not (p / "tokenizer.json").exists():
            raise SystemExit(f"--model {name}: {p} has no model.onnx + tokenizer.json")
        models.append((name, p))
    return models


def detected_types(labels: dict, min_score: float) -> list[str]:
    """Entity types at or above min_score, the unit the router logs report."""
    types = set()
    for label, score in labels.items():
        if label == "O" or score < min_score:
            continue
        for prefix in TAG_PREFIXES:
            if label.startswith(prefix):
                label = label[len(prefix) :]
                break
        types.add(label)
    return sorted(types)


def routes_local(labels: dict, min_score: float) -> bool:
    """The rule every l2_pii_onnx_* policy uses: any non-"O" label >= min_score
    (routing_policy.cpp rejects only value < min_score)."""
    return any(label != "O" and score >= min_score for label, score in labels.items())
