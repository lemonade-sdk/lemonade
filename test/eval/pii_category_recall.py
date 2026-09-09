"""
Coverage-aware per-category recall from existing PII benchmark run logs.

The runs in this series score one binary - "did the model emit any entity at
all" - which is what routing consumes but which flattens every model to a
near-identical recall number. This script re-scores those SAME runs per PII
category, with no new inference: every verbose log already records its
per-case predictions (`detected=CREDIT_CARD,DATE_TIME,...`), so gold and
predicted labels are both recoverable offline.

Both sides are mapped into the canonical taxonomy in pii_taxonomy.py, so a
model is never compared against label names it doesn't use.

What this measures, and what it does NOT
----------------------------------------
MEASURES: per-category recall, restricted to categories a model can actually
express; which categories it structurally cannot see; and how its
document-level misses split between "could have caught it" and "has no class
for it".

DOES NOT MEASURE: precision, false-positive rate, or anything requiring
negative examples. Nemotron-PII is a pure-positive dataset - a scan of 30,000
test rows found ZERO documents without PII - so the corpora here carry a
single benign case and the FP side of every confusion matrix in this series is
statistically empty. A model that flagged every document indiscriminately
would score identically to a precise one on everything reported here. Any
precision claim needs a constructed negative arm, which does not exist yet.

Credit rules (see pii_taxonomy.py for the rationale)
----------------------------------------------------
Default is LENIENT: a coarse prediction earns credit for every gold category
in its expansion, so pplx's single `account_number` can satisfy an SSN, a
customer ID and a card number at once. This matches the routing question -
that document did get flagged - but it flatters coarse taxonomies.

--strict instead solves a maximum bipartite matching between predicted labels
and gold categories, so one emitted label can satisfy at most one gold
category. Neither is "the" right answer without span offsets to disambiguate
against (the raw Nemotron rows have them; the built corpus discards them), so
the report prints both headline numbers and flags where they diverge.

Usage:
    python test/eval/pii_category_recall.py --corpus-dir DIR LOG [LOG ...]

Examples:
    # One model
    python test/eval/pii_category_recall.py \
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k \
        test/conformance/routing/1/l2_pii_nemotron_20k/runs/ner_mmbert32k-pii-detector-merged_20260811-194327.log

    # Compare every model that has been run on the 20k corpus
    python test/eval/pii_category_recall.py \
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k \
        test/conformance/routing/1/l2_pii_nemotron_20k/runs/*.log
"""

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

from pii_taxonomy import CANONICAL, MODELS, coverage, normalize

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass


# Matches the per-case line every eval script in this directory emits. The
# `detected=` group is deliberately anchored last: FN lines carry an
# `expected_pii=` between the name and it, and pplx/gliner lines append
# `sensitivity=` / `type_coverage=` after it.
_CASE_RE = re.compile(
    r"^\s*\[(?:PASS|FAIL)\]\[(TP|TN|FP|FN)\]\s+(\S+?):.*?detected=(\S+)"
)

# Log filename prefixes -> taxonomy key, so a directory of logs can be passed
# as a glob without naming each model.
# Longest prefix first: "ner_privacy-filter-multilingual" (OpenMed) must be
# tested before "ner_privacy-filter_" (openai/privacy-filter), which uses a
# different taxonomy despite the near-identical name.
_PREFIX_TAXONOMY = [
    ("pplx_", "pplx"),
    ("gliner_", "gliner"),
    ("ner_mmbert", "mmbert"),
    ("ner_privacy-filter-multilingual", "openmed"),
    ("onnx_privacy-filter", "openmed"),
    ("ner_privacy-filter_", "openai_pf"),
    ("policy_llm", "llm"),
]

# Outcome-only line, used by --doc-level. Works across BOTH log dialects,
# which disagree about what the second bracket means: the NER scripts put the
# outcome there ([PASS][TP] detected / [FAIL][FN] missed), while
# pii_routing_eval.py puts the CASE CLASS there and carries the outcome in the
# verdict ([PASS][TP] routed local / [FAIL][TP] leaked to cloud). The verdict
# is the one field that means the same thing in both, so it alone is read.
# The optional parenthetical absorbs pii_routing_eval.py's "(pii=a,b,c)",
# which sits between the case name and the colon on FAIL lines only.
_VERDICT_RE = re.compile(
    r"^\s*\[(PASS|FAIL)\]\[(TP|TN|FP|FN)\]\s+(\S+?)(?:\s+\([^)]*\))?:"
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("logs", nargs="+", help="Run log(s) to re-score")
    p.add_argument(
        "--corpus-dir",
        required=True,
        help="Directory containing the cases.jsonl these runs were scored against",
    )
    p.add_argument(
        "--taxonomy",
        default=None,
        choices=sorted(MODELS),
        help="Force a taxonomy for all logs. Default: infer per log from its "
        "filename prefix (pplx_, gliner_, ner_mmbert, ner_privacy-filter).",
    )
    p.add_argument(
        "--strict",
        action="store_true",
        help="Disable coarse-label credit: one emitted label satisfies at most "
        "one gold category (maximum bipartite matching).",
    )
    p.add_argument(
        "--min-support",
        type=int,
        default=0,
        help="Hide categories with fewer than this many gold occurrences in the "
        "corpus. Default 0 - every canonical category is listed, including ones "
        "no model in the comparison can express, so coverage holes stay visible "
        "instead of vanishing from the table.",
    )
    p.add_argument(
        "--doc-level",
        action="store_true",
        help="Score by document routing outcome instead of emitted labels, so "
        "LLM-as-router runs (which emit no labels) can be compared. Reports "
        "miss ENRICHMENT per category, not recall - see the module docstring.",
    )
    p.add_argument("--json-out", default=None, help="Write the full report as JSON")
    return p.parse_args()


def infer_taxonomy(log_path: Path) -> str | None:
    name = log_path.name
    for prefix, key in _PREFIX_TAXONOMY:
        if name.startswith(prefix):
            return key
    return None


def load_gold(corpus_dir: Path) -> dict[str, list[str]]:
    cases_path = corpus_dir / "cases.jsonl"
    if not cases_path.exists():
        print(f"ERROR: cases.jsonl not found at {cases_path}", file=sys.stderr)
        sys.exit(1)
    gold: dict[str, list[str]] = {}
    for line in cases_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        case = json.loads(line)
        raw = case.get("pii_category", "none")
        gold[case.get("name", "?")] = (
            [] if raw == "none" else [t for t in raw.split(",") if t]
        )
    return gold


def parse_log(log_path: Path) -> dict[str, list[str]]:
    """name -> predicted raw labels ([] when the model detected nothing)."""
    preds: dict[str, list[str]] = {}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _CASE_RE.match(line)
        if not m:
            continue
        _, name, detected = m.groups()
        if name in preds:
            continue
        preds[name] = (
            [] if detected == "none" else [t for t in detected.split(",") if t]
        )
    return preds


def gold_support(gold) -> dict[str, int]:
    """Canonical category -> documents containing it, across the whole corpus.

    Computed from the gold labels alone. The per-model tallies can't stand in
    for this: they only count categories a model covers, so a category NO model
    in the comparison can express would otherwise disappear from the report
    entirely - which is exactly the hole worth seeing.
    """
    counts = Counter()
    for labels in gold.values():
        if not labels:
            continue
        canon, _, _ = normalize(labels, MODELS["nemotron"])
        for cat in canon:
            counts[cat] += 1
    return counts


def parse_log_verdicts(log_path: Path) -> dict[str, bool]:
    """name -> True when the model flagged the document, False when it missed."""
    out: dict[str, bool] = {}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _VERDICT_RE.match(line)
        if not m:
            continue
        verdict, _, name = m.groups()
        if name not in out:
            out[name] = verdict == "PASS"
    return out


def score_doc_level(gold, verdicts, taxonomy_key):
    """Per-category miss enrichment from document-level routing outcomes.

    Nemotron documents carry ~7 categories each and only 8 of 2,500 have a
    single one, so a missed document is missed for every category it contains
    and per-category RECALL here would be confounded beyond use. Enrichment -
    P(miss | doc contains c) divided by the model's overall miss rate - is
    robust to that: it asks which categories are over-represented among the
    documents this model let through, which is the blind-spot question the
    "missed categories" column was always really asking. 1.0 means the
    category is missed at exactly the model's baseline rate.
    """
    mapping = MODELS[taxonomy_key]
    covered = coverage(mapping)
    gold_mapping = MODELS["nemotron"]

    support = Counter()
    missed = Counter()
    n_scored = 0
    n_missed = 0

    for name, gold_labels in gold.items():
        if name not in verdicts or not gold_labels:
            continue
        n_scored += 1
        flagged = verdicts[name]
        if not flagged:
            n_missed += 1
        gold_canon, _, _ = normalize(gold_labels, gold_mapping)
        for cat in gold_canon & covered:
            support[cat] += 1
            if not flagged:
                missed[cat] += 1

    base = n_missed / n_scored if n_scored else 0.0
    per_category = {}
    for cat in CANONICAL:
        if not support[cat]:
            continue
        rate = missed[cat] / support[cat]
        per_category[cat] = {
            "support": support[cat],
            "missed": missed[cat],
            "miss_rate": rate,
            "enrichment": (rate / base) if base else None,
        }
    return {
        "taxonomy": taxonomy_key,
        "doc_level": True,
        "n_scored": n_scored,
        "n_missed": n_missed,
        "baseline_miss_rate": base,
        "per_category": per_category,
        "covered_categories": sorted(covered),
        "gap_categories": [],
        "gap_support": {},
        "doc_in_coverage": {"total": n_scored, "missed": n_missed, "miss_rate": base},
        "doc_gap_only": {"total": 0, "missed": 0, "miss_rate": None},
        "catch_all_only_docs": 0,
        "unmapped_labels": [],
    }


def report_doc_level(results, support_counts, min_support) -> None:
    print()
    print("=" * 78)
    print("PER-CATEGORY MISS ENRICHMENT (document-level routing outcome)")
    print("=" * 78)
    print(
        "Enrichment = P(missed | document contains this category) / overall miss rate."
        "\n>1.0 means the category is over-represented among the documents this model"
        "\nlet through. Raw per-category recall is NOT shown: Nemotron documents carry"
        "\n~7 categories each (only 8 of 2,500 have one), so a missed document is"
        "\nmissed for all of them and recall would be confounded. Precision is not"
        "\nmeasured here either - the corpus has one benign case."
    )
    print()
    for r in results:
        print(
            f"  {r['label']}: {r['n_missed']}/{r['n_scored']} documents missed "
            f"(baseline {r['baseline_miss_rate']:.2%})"
        )
    print()

    names = [r["label"] for r in results]
    width = max(max(len(n) for n in names) + 2, 12)
    header = "category".ljust(26) + "support".rjust(9)
    for n in names:
        header += n.rjust(width)
    print(header)
    print("-" * (35 + width * len(names)))

    ordered = sorted(
        [c for c in CANONICAL if support_counts.get(c, 0) >= min_support],
        key=lambda c: -max(
            r["per_category"].get(c, {}).get("enrichment") or 0 for r in results
        ),
    )
    for cat in ordered:
        row = cat.ljust(26) + str(support_counts.get(cat, 0)).rjust(9)
        for r in results:
            entry = r["per_category"].get(cat)
            if cat not in r["covered_categories"]:
                cell = "no class"
            elif not entry or entry["enrichment"] is None:
                cell = "n/a"
            else:
                cell = f"{entry['enrichment']:.2f}x"
            row += cell.rjust(width)
        print(row)
    print()
    print(
        '  "no class" = the category is absent from that model\'s taxonomy'
        ' (a coverage hole, not a score).\n  "n/a" = covered by the'
        " taxonomy, but no gold document in this corpus exercises it."
    )
    print()


def credited_lenient(pred_canon: set[str], gold_canon: set[str]) -> set[str]:
    return gold_canon & pred_canon


def credited_strict(pred_labels, mapping, gold_canon: set[str]) -> set[str]:
    """Maximum bipartite matching: each emitted label satisfies <=1 gold category.

    Kuhn's algorithm; both sides are a handful of nodes per document, so the
    naive augmenting-path search is far cheaper than the setup for anything
    smarter.
    """
    edges: dict[str, list[str]] = {}
    for label in pred_labels:
        targets = mapping.get(label)
        if not targets:
            continue
        hits = [t for t in targets if t in gold_canon]
        if hits:
            edges[label] = hits

    match_right: dict[str, str] = {}

    def assign(node: str, seen: set[str]) -> bool:
        for cat in edges.get(node, ()):
            if cat in seen:
                continue
            seen.add(cat)
            if cat not in match_right or assign(match_right[cat], seen):
                match_right[cat] = node
                return True
        return False

    for label in edges:
        assign(label, set())
    return set(match_right)


def score(gold, preds, taxonomy_key, strict):
    mapping = MODELS[taxonomy_key]
    covered = coverage(mapping)
    gold_mapping = MODELS["nemotron"]

    support = Counter()
    detected = Counter()
    gap_support = Counter()
    unmapped_seen: set[str] = set()

    n_scored = 0
    doc_miss_in_coverage = 0
    doc_total_in_coverage = 0
    doc_miss_gap_only = 0
    doc_total_gap_only = 0
    catch_all_only = 0

    for name, gold_labels in gold.items():
        if name not in preds or not gold_labels:
            continue
        n_scored += 1
        gold_canon, _, gold_unmapped = normalize(gold_labels, gold_mapping)
        unmapped_seen |= {f"gold:{u}" for u in gold_unmapped}

        pred_labels = preds[name]
        pred_canon, saw_catch_all, pred_unmapped = normalize(pred_labels, mapping)
        unmapped_seen |= {f"pred:{u}" for u in pred_unmapped}

        in_scope = gold_canon & covered
        out_of_scope = gold_canon - covered
        for cat in in_scope:
            support[cat] += 1
        for cat in out_of_scope:
            gap_support[cat] += 1

        if strict:
            hit = credited_strict(pred_labels, mapping, in_scope)
        else:
            hit = credited_lenient(pred_canon, in_scope)
        for cat in hit:
            detected[cat] += 1

        fired = bool(pred_canon) or saw_catch_all
        if saw_catch_all and not pred_canon:
            catch_all_only += 1

        # A document whose entire gold set falls outside the model's taxonomy
        # is one it structurally cannot be expected to catch; separating these
        # is the whole point of coverage-aware scoring.
        if in_scope:
            doc_total_in_coverage += 1
            if not fired:
                doc_miss_in_coverage += 1
        else:
            doc_total_gap_only += 1
            if not fired:
                doc_miss_gap_only += 1

    return {
        "taxonomy": taxonomy_key,
        "strict": strict,
        "n_scored": n_scored,
        "covered_categories": sorted(covered),
        "gap_categories": sorted(gap_support),
        "per_category": {
            cat: {
                "support": support[cat],
                "detected": detected[cat],
                "recall": detected[cat] / support[cat] if support[cat] else None,
            }
            for cat in CANONICAL
            if support[cat]
        },
        "gap_support": dict(gap_support),
        "doc_in_coverage": {
            "total": doc_total_in_coverage,
            "missed": doc_miss_in_coverage,
            "miss_rate": (
                doc_miss_in_coverage / doc_total_in_coverage
                if doc_total_in_coverage
                else None
            ),
        },
        "doc_gap_only": {
            "total": doc_total_gap_only,
            "missed": doc_miss_gap_only,
            "miss_rate": (
                doc_miss_gap_only / doc_total_gap_only if doc_total_gap_only else None
            ),
        },
        "catch_all_only_docs": catch_all_only,
        "unmapped_labels": sorted(unmapped_seen),
    }


def report(results, support_counts, min_support) -> None:
    print()
    print("=" * 78)
    print("COVERAGE-AWARE PER-CATEGORY RECALL")
    print("=" * 78)
    print(
        "Recall only. Precision and FP-rate are NOT measured: Nemotron-PII has no\n"
        "benign documents (0 in 30,000 test rows scanned), so the negative arm of\n"
        "every run in this series is n=1. An indiscriminate detector would score\n"
        "the same as a precise one on everything below."
    )

    names = [r["label"] for r in results]
    width = max(len(n) for n in names) + 2

    print()
    print("PER-CATEGORY RECALL")
    print("-" * 78)
    header = "category".ljust(26) + "support".rjust(9)
    for n in names:
        header += n.rjust(width)
    print(header)

    for cat in CANONICAL:
        if support_counts.get(cat, 0) < min_support:
            continue
        row = cat.ljust(26) + str(support_counts.get(cat, 0)).rjust(9)
        for r in results:
            entry = r["per_category"].get(cat)
            if cat not in r["covered_categories"]:
                cell = "no class"
            elif entry and entry["support"]:
                cell = f"{entry['recall']:.1%}"
            else:
                cell = "n/a"
            row += cell.rjust(width)
        print(row)

    print()
    print("DOCUMENT-LEVEL MISSES, SPLIT BY WHETHER THE MODEL COULD SEE THE CATEGORY")
    print("-" * 78)
    for r in results:
        cov = r["doc_in_coverage"]
        gap = r["doc_gap_only"]
        print(f"  {r['label']}  (n={r['n_scored']}, taxonomy={r['taxonomy']})")
        if cov["total"]:
            print(
                f"      in-coverage docs : {cov['missed']:5d} / {cov['total']:5d} missed"
                f"  ({cov['miss_rate']:.2%})   <- genuine misses"
            )
        if gap["total"]:
            print(
                f"      gap-only docs    : {gap['missed']:5d} / {gap['total']:5d} missed"
                f"  ({gap['miss_rate']:.2%})   <- no class for this PII"
            )
        if r["catch_all_only_docs"]:
            print(
                f"      catch-all only   : {r['catch_all_only_docs']:5d} docs flagged "
                "solely by a generic label"
            )
        if r["gap_categories"]:
            print(f"      coverage gaps    : {', '.join(r['gap_categories'])}")
        if r["unmapped_labels"]:
            print(f"      UNMAPPED (fix taxonomy): {', '.join(r['unmapped_labels'])}")
        print()


def main() -> None:
    args = parse_args()
    gold = load_gold(Path(args.corpus_dir))
    support_counts = gold_support(gold)

    results = []
    for log in args.logs:
        log_path = Path(log)
        if not log_path.exists():
            print(f"WARNING: skipping missing log {log_path}", file=sys.stderr)
            continue
        taxonomy_key = args.taxonomy or infer_taxonomy(log_path)
        if taxonomy_key is None:
            print(
                f"WARNING: skipping {log_path.name} - cannot infer taxonomy from "
                "its name; pass --taxonomy",
                file=sys.stderr,
            )
            continue
        if args.doc_level:
            verdicts = parse_log_verdicts(log_path)
            if not verdicts:
                print(
                    f"WARNING: skipping {log_path.name} - no per-case verdict lines",
                    file=sys.stderr,
                )
                continue
            result = score_doc_level(gold, verdicts, taxonomy_key)
            label = taxonomy_key
            if any(r["taxonomy"] == taxonomy_key for r in results):
                label = f"{taxonomy_key}#{sum(r['taxonomy'] == taxonomy_key for r in results) + 1}"
            result["label"] = label
            result["log"] = str(log_path)
            results.append(result)
            continue

        preds = parse_log(log_path)
        if not preds:
            print(
                f"WARNING: skipping {log_path.name} - no per-case lines found "
                "(was the run made with --verbose?)",
                file=sys.stderr,
            )
            continue
        result = score(gold, preds, taxonomy_key, args.strict)
        # The taxonomy key names the model far better than the filename does:
        # a log can be named after a bare HF snapshot hash. Disambiguate only
        # when the same taxonomy appears twice (e.g. openmed v1 and v2).
        label = taxonomy_key
        if any(r["taxonomy"] == taxonomy_key for r in results):
            label = f"{taxonomy_key}#{sum(r['taxonomy'] == taxonomy_key for r in results) + 1}"
        result["label"] = label
        result["log"] = str(log_path)
        results.append(result)

    if not results:
        print("ERROR: no usable logs", file=sys.stderr)
        sys.exit(1)

    if args.doc_level:
        report_doc_level(results, support_counts, args.min_support)
    else:
        report(results, support_counts, args.min_support)

    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        print(f"JSON -> {args.json_out}")


if __name__ == "__main__":
    main()
