"""Lemonade Router demo — one policy, four prompts, watch where each is routed.

Drives a `collection.router` collection with a vanilla OpenAI client. The server
picks a candidate per the policy's first-matching rule (fail-open to
`default_model`) and reports its decision two ways:

  * response header `x-lemonade-route`  -> the matched rule id (or "default")
  * response body `x_lemonade_route`    -> { route_to, matched_rule, default_used,
                                             outputs, trace[] }  (with route_trace=true)

`route_to` is the candidate that actually answered.

Usage:
    python examples/router/demo.py --model user.Demo-Router-Local
    python examples/router/demo.py --model user.Demo-Router-Cloud --base-url http://localhost:13305/api/v1
"""

import argparse

from openai import OpenAI

LONG_PROMPT = "Summarize the following log. " + ("error timeout retry " * 260)  # > 4000 chars

CASES = [
    {
        "name": "casual",
        "prompt": "Give me a fun fact about otters.",
        "metadata": None,
        "expect_route_to": "small/default",
    },
    {
        "name": "coding",
        "prompt": "Write a Python function to reverse a singly linked list.",
        "metadata": None,
        "expect_route_to": "capable (keyword 'function'/'def ')",
    },
    {
        "name": "long-context",
        "prompt": LONG_PROMPT,
        "metadata": None,
        "expect_route_to": "capable (min_chars >= 4000)",
    },
    {
        "name": "coding-but-consent-denied",
        "prompt": "Write a Python function to reverse a singly linked list.",
        "metadata": {"consent": "denied"},
        "expect_route_to": "small/default (privacy rule wins first-match)",
    },
]


def run_case(client, model, case):
    extra_body = {"route_trace": True}
    if case["metadata"] is not None:
        extra_body["metadata"] = case["metadata"]

    raw = client.chat.completions.with_raw_response.create(
        model=model,
        messages=[{"role": "user", "content": case["prompt"]}],
        max_tokens=64,
        temperature=0.0,
        extra_body=extra_body,
    )
    header_route = raw.headers.get("x-lemonade-route", "<missing>")
    body = raw.http_response.json()
    decision = body.get("x_lemonade_route", {})
    answered_by = body.get("model", "<unknown>")

    print(f"\n=== case: {case['name']} ===")
    print(f"  chars in prompt        : {len(case['prompt'])}")
    if case["metadata"]:
        print(f"  metadata               : {case['metadata']}")
    print(f"  expected               : {case['expect_route_to']}")
    print(f"  x-lemonade-route (hdr) : {header_route}")
    print(f"  route_to (body)        : {decision.get('route_to', '<missing>')}")
    print(f"  matched_rule           : {decision.get('matched_rule', '<missing>')}")
    print(f"  default_used           : {decision.get('default_used', '<missing>')}")
    print(f"  outputs                : {decision.get('outputs', {})}")
    print(f"  response 'model' field : {answered_by}")
    trace = decision.get("trace", [])
    if trace:
        print("  trace:")
        for t in trace:
            score = f" score={t['score']:.3f}" if "score" in t else ""
            print(f"    - {t['condition']}: {t['result']}{score}")
    return decision


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="collection.router model name")
    ap.add_argument("--base-url", default="http://localhost:13305/api/v1")
    ap.add_argument("--api-key", default="lemonade")
    args = ap.parse_args()

    client = OpenAI(base_url=args.base_url, api_key=args.api_key)
    print(f"Router demo -> model={args.model!r}  base_url={args.base_url}")
    for case in CASES:
        run_case(client, args.model, case)


if __name__ == "__main__":
    main()
