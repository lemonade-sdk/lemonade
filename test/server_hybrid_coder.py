"""Opt-in, real-server smoke test for Hybrid-Coder-Mode.

Requires a tool-calling GGUF. Optionally set LEMONADE_HYBRID_TEST_SECONDARY_MODEL
for a second model (of a different type, to avoid the LLM LRU limit). The
server must permit both to remain resident. No model is loaded or modified
when LEMONADE_HYBRID_TEST_MODEL is unset.
"""

from __future__ import annotations

import json
import os
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import pytest

MODEL = os.environ.get("LEMONADE_HYBRID_TEST_MODEL")
BASE_URL = os.environ.get("LEMONADE_HYBRID_TEST_URL", "http://127.0.0.1:13305")
KEY = os.environ.get("LEMONADE_ADMIN_API_KEY") or os.environ.get("LEMONADE_API_KEY")
TEST_CTX_SIZE = int(os.environ.get("LEMONADE_HYBRID_TEST_CTX_SIZE", "65536"))
SECONDARY_MODEL = os.environ.get("LEMONADE_HYBRID_TEST_SECONDARY_MODEL")

pytestmark = pytest.mark.skipif(
    not MODEL, reason="set LEMONADE_HYBRID_TEST_MODEL for the real-backend test"
)


def call(path, body=None):
    headers = {"Content-Type": "application/json"}
    if KEY:
        headers["Authorization"] = f"Bearer {KEY}"
    if path == "/v1/chat/completions" and body is not None:
        body = {
            **body,
            "hybrid_coder_mode": True,
            "x_lemonade_hybrid_session": body.get(
                "x_lemonade_hybrid_session", "smoke-primary"
            ),
        }
    request = Request(
        BASE_URL + path,
        data=json.dumps(body).encode() if body is not None else None,
        headers=headers,
        method="POST" if body is not None else "GET",
    )
    try:
        with urlopen(request, timeout=600) as response:
            return response.status, json.loads(response.read())
    except HTTPError as error:
        return error.code, json.loads(error.read())


def stream_call(path, body):
    headers = {"Content-Type": "application/json"}
    if KEY:
        headers["Authorization"] = f"Bearer {KEY}"
    if path == "/v1/chat/completions":
        body = {
            **body,
            "hybrid_coder_mode": True,
            "x_lemonade_hybrid_session": body.get(
                "x_lemonade_hybrid_session", "smoke-primary"
            ),
        }
    req = Request(
        BASE_URL + path, data=json.dumps(body).encode(), headers=headers, method="POST"
    )
    events = []
    done = False
    start = time.perf_counter()
    with urlopen(req, timeout=600) as response:
        status = response.status
        for line in response:
            if not line.startswith(b"data:"):
                continue
            payload = line[5:].strip()
            if payload == b"[DONE]":
                done = True
                break
            event = json.loads(payload)
            assert "error" not in event, event
            events.append((time.perf_counter() - start, event))
    assert done, "Stream was interrupted before [DONE]"
    return status, events


def assemble_stream(events):
    content = ""
    calls = {}
    finish = None
    usage = None
    metadata = None
    content_times = []
    for elapsed, chunk in events:
        if "usage" in chunk:
            usage = chunk["usage"]
        if "x_lemonade_hybrid" in chunk:
            metadata = chunk["x_lemonade_hybrid"]
        choices = chunk.get("choices") or []
        if not choices:
            continue
        choice = choices[0]
        if choice.get("finish_reason") is not None:
            finish = choice["finish_reason"]
        delta = choice.get("delta") or {}
        if isinstance(delta.get("content"), str) and delta["content"]:
            content += delta["content"]
            content_times.append(elapsed)
        for fragment in delta.get("tool_calls", []):
            idx = fragment["index"]
            current = calls.setdefault(
                idx,
                {
                    "id": "",
                    "type": "function",
                    "function": {"name": "", "arguments": ""},
                },
            )
            if fragment.get("id"):
                current["id"] = fragment["id"]
            fn = fragment.get("function") or {}
            current["function"]["name"] += fn.get("name", "")
            current["function"]["arguments"] += fn.get("arguments", "")
    message = {"role": "assistant", "content": content}
    if calls:
        message["tool_calls"] = [calls[idx] for idx in sorted(calls)]
    return message, finish, usage, metadata, content_times


def assert_context_info(meta, slot_size=None):
    assert meta["context_management"] == "server", meta
    ctx = meta["context"]
    assert (
        ctx["management"] == "server"
        and ctx["outer_transcript_is_model_context"] is False
    )
    assert ctx["outer_transcript_limit_bytes"] >= 32 * 1024 * 1024
    for role in ("orchestrator", "worker"):
        entry = ctx[role]
        assert (
            isinstance(entry["capacity_tokens"], int)
            and entry["capacity_tokens"] >= 512
        )
        if slot_size is not None:
            assert (
                entry["capacity_tokens"]
                == slot_size[0 if role == "orchestrator" else 1]
            )
        measured = entry["last_prompt_tokens"]
        assert measured is None or isinstance(measured, int) and measured > 0
        assert entry["measurement"] == (
            "unavailable" if measured is None else "last_completed_inference"
        )


def echo_tool():
    return {
        "type": "function",
        "function": {
            "name": "echo_probe",
            "description": "A harmless test tool. Returns a text echo of its input.",
            "parameters": {
                "type": "object",
                "properties": {"value": {"type": "string"}},
                "required": ["value"],
            },
        },
    }


def test_pi_style_external_tool_roundtrip():
    status, result = call(
        "/v1/load",
        {"model_name": MODEL, "hybrid_coder_mode": True, "ctx_size": TEST_CTX_SIZE},
    )
    assert status == 200, result
    status, slots = call("/v1/slots")
    assert status == 200 and [slot["id"] for slot in slots] == [0, 1], slots
    assert abs(slots[0]["n_ctx"] - slots[1]["n_ctx"]) <= 1, slots

    # A plain Chat Completion on the same Hybrid-loaded model must not
    # get orchestrated. It may use either physical backend slot.
    plain_req = Request(
        BASE_URL + "/v1/chat/completions",
        data=json.dumps(
            {
                "model": MODEL,
                "messages": [{"role": "user", "content": "Say hello."}],
                "max_tokens": 256,
            }
        ).encode(),
        headers={
            "Content-Type": "application/json",
            **({"Authorization": f"Bearer {KEY}"} if KEY else {}),
        },
        method="POST",
    )
    with urlopen(plain_req, timeout=600) as response:
        plain_answer = json.loads(response.read())
        assert "x_lemonade_hybrid" not in plain_answer

    if SECONDARY_MODEL:
        assert (
            SECONDARY_MODEL != MODEL
        ), "secondary model must differ from the hybrid model"
        status, second = call("/v1/load", {"model_name": SECONDARY_MODEL})
        assert status == 200, second
        status, health = call("/v1/health")
        assert status == 200, health
        loaded_names = {
            entry.get("model_name")
            for entry in health.get("all_models_loaded", [])
            if isinstance(entry, dict)
        }
        assert MODEL in loaded_names and SECONDARY_MODEL in loaded_names, (
            "Both models must remain loaded. Choose another model type or "
            "increase the configured model residency limit.",
            loaded_names,
        )
        # The secondary model may be the most-recent backend. The hybrid
        # request below must still query/erase only MODEL's slots.

    # Kilo Code and other OpenAI clients may send several leading system and
    # developer messages. These must become ONE system message for llama.cpp.
    history = [
        {"role": "system", "content": "You are a coding assistant."},
        {
            "role": "developer",
            "content": "Use only the tools provided in this request.",
        },
        {
            "role": "system",
            "content": [
                {"type": "text", "text": "Honor the user's coding task."},
            ],
        },
        {
            "role": "user",
            "content": (
                "Use hybrid_delegate_task to delegate this entire task to the worker: "
                "call the provided echo_probe tool with value 'probe', then report "
                "its literal result. Do not call echo_probe as the orchestrator."
            ),
        },
    ]
    external_calls = 0
    last_hybrid = {}
    for _ in range(8):
        body = {
            "model": MODEL,
            "messages": history,
            "tools": [echo_tool()],
            "temperature": 0,
            "max_tokens": 1024,
        }
        if external_calls == 0:
            status, events = stream_call(
                "/v1/chat/completions",
                {**body, "stream": True, "stream_options": {"include_usage": True}},
            )
            assert status == 200, events
            message, finish, usage, metadata, _ = assemble_stream(events)
            assert finish in ("stop", "tool_calls"), finish
            assert usage is not None and usage["total_tokens"] > 0, usage
            assert metadata is not None, events
            last_hybrid = metadata
        else:
            status, result = call("/v1/chat/completions", {**body, "stream": False})
            assert status == 200, result
            last_hybrid = result["x_lemonade_hybrid"]
            message = result["choices"][0]["message"]
        assert_context_info(last_hybrid, [slot["n_ctx"] for slot in slots])
        history.append(message)
        tool_calls = message.get("tool_calls") or []
        if not tool_calls:
            break
        if external_calls == 0 and tool_calls:
            # A second Hybrid session can run while the first one has pending
            # external tool results. Switching back must restore the first.
            status, other = call(
                "/v1/chat/completions",
                {
                    "model": MODEL,
                    "x_lemonade_hybrid_session": "smoke-other",
                    "messages": [
                        {"role": "user", "content": "One sentence about Python."}
                    ],
                    "max_tokens": 2048,
                },
            )
            assert status == 200, other
        for item in tool_calls:
            assert item["function"]["name"] == "echo_probe", item
            args = json.loads(item["function"]["arguments"])
            history.append(
                {
                    "role": "tool",
                    "tool_call_id": item["id"],
                    "content": args["value"],
                }
            )
            external_calls += 1
    else:
        pytest.fail("Hybrid tool loop did not finish in 8 external requests")

    assert external_calls >= 1, "Model did not delegate and use the client's tool"
    assert last_hybrid["worker_generations"] >= 1
    assert last_hybrid["orchestrator_prompt_tokens"] > 0
    assert last_hybrid["internal_prompt_tokens_total"] > 0
    assert "probe" in str(history[-1].get("content", "")).lower()

    # A client can return the same tool messages with equivalent wire encoding.
    # This must not change the model's authoritative conversation.
    for message in history:
        if message.get("role") == "assistant" and message.get("tool_calls"):
            message["content"] = None
            for tool in message["tool_calls"]:
                arguments = json.loads(tool["function"]["arguments"])
                tool["function"]["arguments"] = json.dumps(
                    arguments, ensure_ascii=False, sort_keys=True, indent=2
                )
        elif message.get("role") == "tool" and isinstance(message.get("content"), str):
            message["content"] = [{"type": "text", "text": message["content"]}]

    # Test actual visible token delivery, not replay of a buffered response.
    history.append(
        {
            "role": "user",
            "content": (
                "Without using tools or delegation, explain Python exception handling "
                "in about 500 words. Include examples and a short checklist."
            ),
        }
    )
    status, events = stream_call(
        "/v1/chat/completions",
        {
            "model": MODEL,
            "messages": history,
            "stream": True,
            "temperature": 0,
            "max_tokens": 1152,
            "stream_options": {"include_usage": True},
        },
    )
    assert status == 200, events
    streamed, reason, usage, metadata, times = assemble_stream(events)
    assert reason in ("stop", "length"), reason
    assert metadata is not None and metadata["orchestrator_prompt_tokens"] > 0
    assert_context_info(metadata, [slot["n_ctx"] for slot in slots])
    assert usage is not None and usage["total_tokens"] >= usage["completion_tokens"] > 0
    assert len(times) > 2, "Expected more than two streamed content deltas"
    assert times[-1] - times[0] > 0.05, "Content was delivered as a post-hoc burst"
    history.append(streamed)

    # With no external tools, the orchestrator answers directly. A small output
    # limit is a valid partial reply, not an HTTP 400 or a fake finish_reason=stop.
    history.append(
        {"role": "user", "content": "Count from 1 to 1000 with explanations."}
    )
    status, result = call(
        "/v1/chat/completions",
        {
            "model": MODEL,
            "messages": history,
            "max_tokens": 4,
            "stream": False,
            "temperature": 0,
        },
    )
    assert status == 200, result
    assert result["choices"][0]["finish_reason"] in ("length", "stop"), result
    assert result["x_lemonade_hybrid"]["routing_fallback"] == "no_client_tools"
    history.append(result["choices"][0]["message"])

    history.append({"role": "user", "content": "Count from 1 to 1000 again."})
    status, events = stream_call(
        "/v1/chat/completions",
        {
            "model": MODEL,
            "messages": history,
            "max_tokens": 4,
            "stream": True,
            "temperature": 0,
            "stream_options": {"include_usage": True},
        },
    )
    assert status == 200, events
    short_reply, reason, usage, metadata, _ = assemble_stream(events)
    assert reason in ("length", "stop"), reason
    assert metadata["routing_fallback"] == "no_client_tools"
    assert usage is not None and usage["total_tokens"] > 0
    history.append(short_reply)

    # Changing a true tool result is not a cosmetic re-encoding.
    tool_index = next(
        (i for i, msg in enumerate(history) if msg.get("role") == "tool"), None
    )
    assert tool_index is not None
    changed_tool = json.loads(json.dumps(history))
    changed_tool[tool_index]["content"] = "REWRITTEN_TOOL_RESULT"
    changed_tool.append({"role": "user", "content": "Continue."})
    status, result = call(
        "/v1/chat/completions", {"model": MODEL, "messages": changed_tool}
    )
    assert status >= 400 and "error" in result, result
    assert "content" in str(result), result

    # A changed prefix must fail, never feed old worker results to orchestrator.
    corrupted = json.loads(json.dumps(history))
    corrupted[0]["content"] += " CHANGED"
    corrupted.append({"role": "user", "content": "Continue."})
    status, result = call(
        "/v1/chat/completions", {"model": MODEL, "messages": corrupted}
    )
    assert status >= 400, result
    assert "error" in result

    # Prefix integrity after a successful external tool execution: formatting
    # differences above are accepted, but actual changes must still fail closed.
    assistant_idx = next(
        i
        for i, msg in enumerate(history)
        if msg.get("role") == "assistant" and msg.get("tool_calls")
    )

    def assert_rejected_history(altered):
        altered.append({"role": "user", "content": "Continue."})
        status, response = call(
            "/v1/chat/completions", {"model": MODEL, "messages": altered}
        )
        assert status >= 400 and "error" in response, response

    invalid = json.loads(json.dumps(history))
    invalid[assistant_idx]["tool_calls"][0]["function"]["arguments"] = "{broken json"
    assert_rejected_history(invalid)

    changed_id = json.loads(json.dumps(history))
    changed_id[assistant_idx]["tool_calls"][0]["id"] = "different-id"
    assert_rejected_history(changed_id)

    changed_name = json.loads(json.dumps(history))
    changed_name[assistant_idx]["tool_calls"][0]["function"]["name"] = "different-tool"
    assert_rejected_history(changed_name)

    changed_value = json.loads(json.dumps(history))
    call_args = changed_value[assistant_idx]["tool_calls"][0]["function"]
    arg_object = json.loads(call_args["arguments"])
    arg_object["value"] = "tampered"
    call_args["arguments"] = json.dumps(arg_object)
    assert_rejected_history(changed_value)

    missing_result = json.loads(json.dumps(history))
    del missing_result[tool_index]
    assert_rejected_history(missing_result)

    duplicate_result = json.loads(json.dumps(history))
    duplicate_result.insert(tool_index + 1, duplicate_result[tool_index].copy())
    assert_rejected_history(duplicate_result)

    unexpected_result = json.loads(json.dumps(history))
    unexpected_result.append(
        {"role": "tool", "tool_call_id": "never-requested", "content": "injected"}
    )
    assert_rejected_history(unexpected_result)

    # Kilo can prune old completed tool results without a summarization call.
    pruned = json.loads(json.dumps(history))
    pruned[tool_index]["content"] = "[Old tool result content cleared]"
    pruned.append({"role": "user", "content": "Give a one-sentence conclusion."})
    status, response = call(
        "/v1/chat/completions",
        {
            "model": MODEL,
            "messages": pruned,
            "max_tokens": 2048,
        },
    )
    assert status == 200, response
    pruned.append(response["choices"][0]["message"])

    # A manual compaction with a stable session ID must preserve the last two
    # messages exactly; the private Orchestrator/Worker context is not replaced.
    compacted = [
        pruned[0],
        {"role": "user", "content": "Earlier context summary"},
        *pruned[-2:],
        {"role": "user", "content": "Continue briefly."},
    ]
    status, response = call(
        "/v1/chat/completions",
        {
            "model": MODEL,
            "messages": compacted,
            "max_tokens": 2048,
        },
    )
    assert status == 200, response
