"""
Prepare an LLM-as-router benchmark run for a given local (privacy) model.

pii_routing_eval.py scores each case by comparing the router's decision to
`decision.route_to` in cases.jsonl, and the committed Nemotron corpora hard-code
Qwen3.5-0.8B-GGUF as that target. Running any other router model (2B, 9B, an
FLM build) against them therefore fails every case unless the corpus is
rewritten to name that model. This script writes a run directory containing:

  cases.jsonl      copy of the source corpus with route_to rewritten
  stats.json       copy with privacy_model / cloud_model updated
  policy_llm_<local-model>.json
                   the LLM router policy, derived from the canonical
                   l2_pii_regex/policy_llm.json prompt with the model names
                   substituted (the category text is left untouched so every
                   run shares the same prompt). Named after the model so the
                   eval's log files (<policy stem>_<timestamp>.log) say which
                   router produced them.

Usage:
    python test/eval/prepare_llm_router_run.py --local-model Qwen3.5-2B-GGUF --out-dir <dir>
    python test/eval/prepare_llm_router_run.py --local-model qwen3.5-0.8b-FLM --cloud-model fireworks.kimi-k2p6 --out-dir <dir>
"""

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CORPUS = REPO / "test/conformance/routing/1/l2_pii_nemotron_20k"
CANONICAL_POLICY = REPO / "test/conformance/routing/1/l2_pii_regex/policy_llm.json"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument(
        "--local-model",
        required=True,
        help="Router + privacy model id, e.g. Qwen3.5-2B-GGUF",
    )
    p.add_argument(
        "--cloud-model", default="fireworks.kimi-k2p6", help="Cloud candidate id"
    )
    p.add_argument("--corpus-dir", type=Path, default=DEFAULT_CORPUS)
    p.add_argument(
        "--policy",
        type=Path,
        default=CANONICAL_POLICY,
        help="Policy whose prompt is reused",
    )
    p.add_argument("--out-dir", type=Path, required=True)
    args = p.parse_args()

    base = json.loads(args.policy.read_text(encoding="utf-8"))
    routing = base["routing"]
    src_local = routing["default_model"]
    src_cloud = next(c for c in routing["candidates"] if c != src_local)

    policy = json.loads(json.dumps(base))
    policy["model_name"] = f"user.PII-LLM-Router-{args.local_model}"
    policy["components"] = [args.cloud_model, args.local_model]
    policy["routing"]["candidates"] = [args.local_model, args.cloud_model]
    policy["routing"]["default_model"] = args.local_model
    policy["routing"]["router"]["model"] = args.local_model
    prompt = policy["routing"]["router"]["prompt"]
    if src_local not in prompt or src_cloud not in prompt:
        print(
            f"ERROR: prompt in {args.policy} does not mention {src_local} and {src_cloud}",
            file=sys.stderr,
        )
        return 1
    policy["routing"]["router"]["prompt"] = prompt.replace(
        src_local, args.local_model
    ).replace(src_cloud, args.cloud_model)

    stats = json.loads((args.corpus_dir / "stats.json").read_text(encoding="utf-8"))
    rename = {
        stats["privacy_model"]: args.local_model,
        stats["cloud_model"]: args.cloud_model,
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    with (args.corpus_dir / "cases.jsonl").open(encoding="utf-8") as src, (
        args.out_dir / "cases.jsonl"
    ).open("w", encoding="utf-8") as dst:
        for line in src:
            case = json.loads(line)
            target = rename[case["decision"]["route_to"]]
            case["decision"]["route_to"] = target
            counts[target] = counts.get(target, 0) + 1
            dst.write(json.dumps(case, ensure_ascii=False) + "\n")

    stats["privacy_model"] = args.local_model
    stats["cloud_model"] = args.cloud_model
    (args.out_dir / "stats.json").write_text(
        json.dumps(stats, indent=2) + "\n", encoding="utf-8"
    )
    policy_name = f"policy_llm_{args.local_model}.json"
    (args.out_dir / policy_name).write_text(
        json.dumps(policy, indent=2) + "\n", encoding="utf-8"
    )

    print(f"Run dir     : {args.out_dir}")
    print(
        f"Policy      : {policy_name}  router={args.local_model}  cloud={args.cloud_model}"
    )
    print(f"route_to    : {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
