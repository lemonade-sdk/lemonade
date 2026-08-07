"""
MCP gateway smoke tests — exercises each of the 5 MCP tools end-to-end.

Two modes:

* **Fast mode (default, used in CI)** — exercises each tool's dispatcher and
  validation path. Only downloads a ~180 MB GGUF for the chat tool. Whisper,
  Stable Diffusion, and Omni are checked via their structured-error paths so
  the workflow stays runnable on a vanilla ubuntu-latest runner.

* **Live mode (local opt-in)** — per-tool flags trigger real inference using
  the models recommended in `docs/api/mcp.md`. Use these when you want
  end-to-end confidence on your own machine without burning GitHub minutes.

Usage:
    # Fast mode (CI default)
    python test/server_mcp_smoke.py

    # Run every tool live with the doc-recommended models (multi-GB download)
    python test/server_mcp_smoke.py --full

    # Opt into individual tools
    python test/server_mcp_smoke.py --live-transcribe --live-image
    python test/server_mcp_smoke.py --live-omni --omni-model LMX-Omni-5.5B-Lite

    # Override model choices
    python test/server_mcp_smoke.py --live-transcribe --transcribe-model Whisper-Tiny

Exit code:
    0 — all selected tools passed
    1 — at least one tool failed (or the server never came up)
"""

import argparse
import base64
import io
import json
import sys
import time
import wave

import requests

# Defaults match docs/api/mcp.md.
TINY_MODEL = "Tiny-Test-Model-GGUF"
DEFAULT_CHAT_MODEL = "Qwen3-1.7B-GGUF"
DEFAULT_TRANSCRIBE_MODEL = "Whisper-Large-v3-Turbo"
DEFAULT_IMAGE_MODEL = "SDXL-Turbo"
DEFAULT_OMNI_MODEL = "LMX-Omni-5.5B-Lite"

# Live tools can pull multi-GB models and run real inference; give them room.
LIVE_PULL_TIMEOUT = 1800  # 30 min
LIVE_CALL_TIMEOUT = 900  # 15 min

# ---------------------------------------------------------------------------
# Output helpers — keep the smoke log readable.
# ---------------------------------------------------------------------------

BAR = "-" * 72
DBAR = "=" * 72


def section(title):
    print()
    print(BAR)
    print(f" {title}")
    print(BAR)


def step(msg):
    print(f"  {msg}")


def detail(msg):
    print(f"      {msg}")


def truncate(s, n=140):
    s = str(s)
    if len(s) <= n:
        return s
    return s[: n - 3] + "..."


def _post(url, payload, timeout=60):
    return requests.post(url, json=payload, timeout=timeout)


def wait_ready(base_url, timeout_s=120):
    deadline = time.time() + timeout_s
    last_err = None
    while time.time() < deadline:
        try:
            r = requests.get(f"{base_url}/api/v1/health", timeout=2)
            if r.status_code == 200:
                return True
            last_err = f"HTTP {r.status_code}"
        except requests.RequestException as e:
            last_err = str(e)
        time.sleep(2)
    print(f"  ! server not ready after {timeout_s}s: {last_err}")
    return False


def pull_model(base_url, model_name, timeout=600):
    step(f"pulling {model_name} ...")
    r = _post(f"{base_url}/api/v1/pull", {"model_name": model_name}, timeout=timeout)
    if r.status_code != 200:
        detail(f"WARN: pull returned {r.status_code}: {truncate(r.text, 200)}")
        return False
    detail(f"{model_name} ready")
    return True


def tools_call(mcp_url, name, arguments, timeout=120):
    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    }
    r = _post(mcp_url, payload, timeout=timeout)
    if r.status_code != 200:
        raise AssertionError(f"HTTP {r.status_code}: {r.text[:300]}")
    body = r.json()
    if "result" not in body:
        raise AssertionError(f"no result in response: {body}")
    return body["result"]


def assert_text_content(result):
    assert "content" in result, f"no content: {result}"
    content = result["content"]
    assert isinstance(content, list) and content, f"empty content: {result}"
    block = content[0]
    assert block.get("type") == "text", f"non-text block: {block}"
    text = block.get("text")
    assert isinstance(text, str), f"non-string text: {block}"
    return text


def call_with_async_retry(mcp_url, name, arguments, timeout, max_wait_s=1800):
    """Wrap tools_call with the async-preparation retry loop documented in
    docs/api/mcp.md. Multi-GB models return isError=true with a 'preparing'
    status block while download/load runs in the background."""
    deadline = time.time() + max_wait_s
    while True:
        result = tools_call(mcp_url, name, arguments, timeout=timeout)
        if result.get("isError") is not True:
            return result
        # Inspect the second content block; the async pattern stuffs a JSON
        # status object there.
        content = result.get("content") or []
        status = None
        if len(content) >= 2 and content[1].get("type") == "text":
            try:
                status = json.loads(content[1].get("text", ""))
            except (ValueError, TypeError):
                status = None
        if not status or status.get("status") != "preparing":
            return result  # genuine error — bubble up
        if time.time() >= deadline:
            raise AssertionError(
                f"{name} still preparing after {max_wait_s}s: {status}"
            )
        retry_after = max(5, int(status.get("retry_after_seconds", 15)))
        print(
            f"      preparing ({status.get('elapsed_seconds', '?')}s elapsed); "
            f"retrying in {retry_after}s ..."
        )
        time.sleep(retry_after)


def make_silent_wav(duration_s=1, sample_rate=16000):
    """Generate a tiny mono 16-bit PCM WAV of silence to feed to Whisper."""
    n_frames = duration_s * sample_rate
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"\x00\x00" * n_frames)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Fast-mode smokes (default; what CI runs).
# ---------------------------------------------------------------------------


def smoke_list_models(mcp_url, _cfg):
    """lemonade_list_models: must return isError=false with a summary block."""
    result = tools_call(mcp_url, "lemonade_list_models", {})
    assert result.get("isError") is False, f"isError: {result}"
    text = assert_text_content(result)
    assert text, "empty summary"
    detail(f"summary: {truncate(text, 100)}")


def smoke_chat(mcp_url, cfg):
    """lemonade_chat: one-token completion against the chat model."""
    model = cfg["chat_model"]
    result = call_with_async_retry(
        mcp_url,
        "lemonade_chat",
        {
            "model": model,
            "messages": [{"role": "user", "content": "Say hi in one word."}],
            "max_tokens": 8,
        },
        timeout=LIVE_CALL_TIMEOUT,
    )
    assert result.get("isError") is False, f"isError: {result}"
    text = assert_text_content(result)
    detail(f"reply: {text!r}")


def smoke_transcribe_dispatch(mcp_url, _cfg):
    """Missing audio input must surface as a structured isError=true."""
    result = tools_call(mcp_url, "lemonade_transcribe_audio", {"model": "Whisper-Tiny"})
    assert result.get("isError") is True, f"expected isError: {result}"
    text = assert_text_content(result)
    detail(f"structured error: {truncate(text, 120)}")


def smoke_generate_image_dispatch(mcp_url, _cfg):
    """Missing prompt/model must surface as a structured isError=true."""
    result = tools_call(mcp_url, "lemonade_generate_image", {})
    assert result.get("isError") is True, f"expected isError: {result}"
    text = assert_text_content(result)
    detail(f"structured error: {truncate(text, 120)}")


def smoke_omni_dispatch(mcp_url, _cfg):
    """A non-collection model must short-circuit with a structured isError."""
    result = tools_call(
        mcp_url,
        "lemonade_omni",
        {
            "model": TINY_MODEL,
            "messages": [{"role": "user", "content": "hi"}],
        },
    )
    assert result.get("isError") is True, f"expected isError: {result}"
    text = assert_text_content(result)
    assert "omni" in text.lower(), f"unexpected text: {text}"
    detail(f"structured error: {truncate(text, 120)}")


# ---------------------------------------------------------------------------
# Live-mode smokes (opt-in via --live-* / --full).
# ---------------------------------------------------------------------------


def smoke_transcribe_live(mcp_url, cfg):
    """Real Whisper transcription against a 1s silent WAV.

    Uses `audio_base64` so the test works regardless of where the server runs
    (native, Docker container, remote host) — `audio_path` is resolved on the
    server's filesystem and would fail when the smoke runs on a different host
    than the server.
    """
    model = cfg["transcribe_model"]
    wav_bytes = make_silent_wav(duration_s=1)
    audio_b64 = base64.b64encode(wav_bytes).decode("ascii")
    result = call_with_async_retry(
        mcp_url,
        "lemonade_transcribe_audio",
        {"model": model, "audio_base64": audio_b64},
        timeout=LIVE_CALL_TIMEOUT,
    )
    assert result.get("isError") is False, f"isError: {result}"
    text = assert_text_content(result)
    # Silence may produce empty or whisper-marker text — both are fine; we
    # just want a successful round trip.
    detail(f"transcript: {truncate(text, 120)!r}")


def smoke_generate_image_live(mcp_url, cfg):
    """Real SD image generation, returned as an inline base64 image block.

    Uses inline content instead of `output_path` so the smoke works regardless
    of where the server runs (native, Docker, remote) — server-side paths
    aren't reachable from a different host.
    """
    model = cfg["image_model"]
    result = call_with_async_retry(
        mcp_url,
        "lemonade_generate_image",
        {
            "model": model,
            "prompt": "a small yellow lemon on a white background",
            "size": "512x512",
        },
        timeout=LIVE_CALL_TIMEOUT,
    )
    assert result.get("isError") is False, f"isError: {result}"
    content = result.get("content") or []
    image_blocks = [b for b in content if b.get("type") == "image"]
    assert image_blocks, f"no image block in response: {str(content)[:200]}"
    data = image_blocks[0].get("data") or ""
    raw_size = (len(data) * 3) // 4  # base64 -> bytes (approx)
    assert raw_size > 1024, f"image suspiciously small (~{raw_size} bytes)"
    mime = image_blocks[0].get("mimeType", "?")
    detail(f"got {mime} image, ~{raw_size:,} bytes")


def smoke_omni_live(mcp_url, cfg):
    """Real Omni collection run, returning artifacts inline as MCP content
    blocks. Uses inline output (no `output_dir`) for portability across
    native/Docker/remote server topologies.
    """
    model = cfg["omni_model"]
    result = call_with_async_retry(
        mcp_url,
        "lemonade_omni",
        {
            "model": model,
            "messages": [
                {
                    "role": "user",
                    "content": (
                        "Generate one small image of a lemon, then say"
                        " 'done' in one word."
                    ),
                }
            ],
        },
        timeout=LIVE_CALL_TIMEOUT,
    )
    assert result.get("isError") is False, f"isError: {result}"
    content = result.get("content") or []
    media = [b for b in content if b.get("type") in ("image", "audio")]
    text_blocks = [b for b in content if b.get("type") == "text"]
    # Either we got media artifacts, or at minimum a non-empty text response.
    assert media or (
        text_blocks and text_blocks[0].get("text")
    ), f"no artifacts and no text in response: {str(content)[:200]}"
    for b in media:
        data = b.get("data") or ""
        raw_size = (len(data) * 3) // 4
        detail(
            f"{b.get('type')} artifact ({b.get('mimeType', '?')}), "
            f"~{raw_size:,} bytes"
        )
    if text_blocks:
        detail(f"text: {truncate(text_blocks[0].get('text', ''), 120)!r}")


# ---------------------------------------------------------------------------
# Runner.
# ---------------------------------------------------------------------------


def build_plan(args):
    """Return (smokes, models_to_pull, cfg). Order matters for output."""
    cfg = {
        "chat_model": args.chat_model,
        "transcribe_model": args.transcribe_model,
        "image_model": args.image_model,
        "omni_model": args.omni_model,
    }

    live_chat = args.full or args.live_chat
    live_transcribe = args.full or args.live_transcribe
    live_image = args.full or args.live_image
    live_omni = args.full or args.live_omni

    smokes = [("lemonade_list_models", smoke_list_models)]

    # Chat: live mode uses the doc-recommended model; fast mode uses tiny.
    if live_chat:
        smokes.append(("lemonade_chat (live)", smoke_chat))
    else:
        cfg["chat_model"] = TINY_MODEL
        smokes.append(("lemonade_chat", smoke_chat))

    smokes.append(
        (
            (
                "lemonade_transcribe_audio (live)"
                if live_transcribe
                else "lemonade_transcribe_audio"
            ),
            smoke_transcribe_live if live_transcribe else smoke_transcribe_dispatch,
        )
    )
    smokes.append(
        (
            (
                "lemonade_generate_image (live)"
                if live_image
                else "lemonade_generate_image"
            ),
            smoke_generate_image_live if live_image else smoke_generate_image_dispatch,
        )
    )
    smokes.append(
        (
            "lemonade_omni (live)" if live_omni else "lemonade_omni",
            smoke_omni_live if live_omni else smoke_omni_dispatch,
        )
    )

    # Models to pre-pull. Tools will lazy-load on first call too, but explicit
    # pulls give nicer progress and let pull failures surface before tool calls.
    pulls = []
    if not args.skip_pull:
        # Chat path always needs *something* loadable.
        pulls.append(cfg["chat_model"])
        if live_transcribe:
            pulls.append(cfg["transcribe_model"])
        if live_image:
            pulls.append(cfg["image_model"])
        if live_omni:
            pulls.append(cfg["omni_model"])

    return smokes, pulls, cfg


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=13305)
    parser.add_argument(
        "--skip-pull",
        action="store_true",
        help="Skip explicit model pulls (tools will still lazy-load).",
    )

    live = parser.add_argument_group(
        "live mode",
        "Run real inference instead of the dispatcher-only checks. Local use "
        "only — these flags trigger multi-GB downloads.",
    )
    live.add_argument(
        "--full",
        action="store_true",
        help="Enable live mode for every tool (equivalent to all --live-* flags).",
    )
    live.add_argument(
        "--live-chat",
        action="store_true",
        help=f"Use the doc-recommended chat model ({DEFAULT_CHAT_MODEL}) instead of {TINY_MODEL}.",
    )
    live.add_argument(
        "--live-transcribe",
        action="store_true",
        help=f"Real Whisper transcription against a 1s silent WAV (default model: {DEFAULT_TRANSCRIBE_MODEL}).",
    )
    live.add_argument(
        "--live-image",
        action="store_true",
        help=f"Real image generation to a temp PNG (default model: {DEFAULT_IMAGE_MODEL}).",
    )
    live.add_argument(
        "--live-omni",
        action="store_true",
        help=f"Real Omni collection run to a temp dir (default model: {DEFAULT_OMNI_MODEL}).",
    )

    overrides = parser.add_argument_group("model overrides")
    overrides.add_argument("--chat-model", default=DEFAULT_CHAT_MODEL)
    overrides.add_argument("--transcribe-model", default=DEFAULT_TRANSCRIBE_MODEL)
    overrides.add_argument("--image-model", default=DEFAULT_IMAGE_MODEL)
    overrides.add_argument("--omni-model", default=DEFAULT_OMNI_MODEL)

    args = parser.parse_args()

    base_url = f"http://{args.host}:{args.port}"
    mcp_url = f"{base_url}/mcp"

    smokes, pulls, cfg = build_plan(args)

    # ----- Header ---------------------------------------------------------
    section("MCP smoke test")
    step(f"target  : {base_url}")
    step(f"plan    : {len(smokes)} test(s)")
    for i, (name, _) in enumerate(smokes, 1):
        detail(f"{i}. {name}")
    if pulls:
        step(f"pre-pull: {', '.join(pulls)}")
    else:
        step("pre-pull: (skipped)")

    # ----- Setup ----------------------------------------------------------
    section("Setup")
    step("waiting for server ready ...")
    if not wait_ready(base_url):
        return 1
    detail("server is up")

    for model in pulls:
        # Big models can take a long time; bump timeout for live mode.
        timeout = LIVE_PULL_TIMEOUT if model != TINY_MODEL else 600
        pull_model(base_url, model, timeout=timeout)

    # ----- Tests ----------------------------------------------------------
    results = []  # list of (name, status, elapsed_s, err_or_none)
    for i, (name, fn) in enumerate(smokes, 1):
        section(f"[{i}/{len(smokes)}] {name}")
        t0 = time.time()
        try:
            fn(mcp_url, cfg)
            elapsed = time.time() - t0
            step(f"PASS  ({elapsed:.1f}s)")
            results.append((name, "PASS", elapsed, None))
        except Exception as e:
            elapsed = time.time() - t0
            step(f"FAIL  ({elapsed:.1f}s)")
            detail(f"reason: {truncate(e, 300)}")
            results.append((name, "FAIL", elapsed, str(e)))

    # ----- Summary --------------------------------------------------------
    print()
    print(DBAR)
    print(" Summary")
    print(DBAR)
    name_w = max(len(n) for n, *_ in results)
    for name, status, elapsed, _ in results:
        marker = "PASS" if status == "PASS" else "FAIL"
        print(f"  {marker:4}  {name:<{name_w}}  {elapsed:6.1f}s")
    print(DBAR)
    passed = sum(1 for _, s, *_ in results if s == "PASS")
    total = len(results)
    failed = [name for name, s, *_ in results if s == "FAIL"]
    overall = "ALL PASS" if not failed else f"{len(failed)} FAILED"
    print(f"  {passed}/{total} passed  --  {overall}")
    if failed:
        print(f"  failed: {', '.join(failed)}")
    print(DBAR)
    print()
    return 1 if failed else 0


if __name__ == "__main__":
    # Line-buffered stdout so section headers appear before each test runs
    # instead of being held in a block buffer until the script exits.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass
    sys.exit(main())
