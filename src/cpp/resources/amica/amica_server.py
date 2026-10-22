"""OpenAI-compatible HTTP server for AMICA ChatApp model bundles.

Lemonade launches this script as the subprocess behind the ``amica`` recipe.
Prompts are templated and tokenized here with the AMICA ChatApp frontend. The
model itself runs in amica-engine, a native process that keeps the bundle's
prefill, decoder and LM-head graphs resident on the GPU through the public
AMICA API; this server starts it once, describes the bundle to it as a
generation plan, and streams the token IDs it returns back as text.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ENTRY_POINT = re.compile(
    r"cgc\.entry_point\s+@[^(]+\((.*?)\)\s*->\s*(.*?)(?:\s+attributes\s+\{|\s*\{)",
    re.S,
)
ENTRY_INPUT = re.compile(r"%[A-Za-z0-9_.$-]+\s*:\s*(!cgc\.(?:tensor<[^>]+>|null))")
ENTRY_OUTPUT = re.compile(r"!cgc\.(?:tensor<[^>]+>|null)")
ENGINE_PREFIX = "@@amica-engine "

DEFAULT_MAX_NEW_TOKENS = 256
EXTERNAL_WEIGHTS_MIN_BYTES = 1 << 20


class RequestError(Exception):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.status = status


class EngineExited(RequestError):
    def __init__(self, message: str):
        super().__init__(message, 500)


def log(message: str) -> None:
    print(f"[amica-server] {message}", file=sys.stderr, flush=True)


def entry_point_abi(graph: Path) -> tuple[list[str], list[str]]:
    match = ENTRY_POINT.search(graph.read_text(encoding="utf-8", errors="replace"))
    if not match:
        raise RuntimeError(f"Cannot read entry-point ABI: {graph}")
    inputs = [re.sub(r"\s+", "", m) for m in ENTRY_INPUT.findall(match.group(1))]
    outputs = [re.sub(r"\s+", "", m) for m in ENTRY_OUTPUT.findall(match.group(2))]
    if not inputs or not outputs:
        raise RuntimeError(f"Entry-point ABI has no inputs or outputs: {graph}")
    return inputs, outputs


def tensor_dims(tensor_type: str) -> list[int]:
    inner = tensor_type[tensor_type.find("<") + 1 : tensor_type.rfind(">")]
    return [int(part) for part in inner.split("x") if part.isdigit()]


def parameter_index(name: str) -> int:
    if not re.fullmatch(r"arg\d+", name or ""):
        raise RuntimeError(f"Expected an argN graph parameter name, got '{name}'")
    return int(name[3:])


def cache_output_map(prefill: Path, decoder: Path) -> tuple[dict[str, str], list[str]]:
    """Bind prefill cache outputs to the decoder inputs with identical types.

    Mirrors the ChatApp runner: caches are located by an exact shape/type
    sequence rather than by position, because some graphs place weights after
    the runtime inputs and others append non-cache outputs after the caches.
    """
    _, prefill_outputs = entry_point_abi(prefill)
    decoder_inputs, _ = entry_point_abi(decoder)
    candidates = prefill_outputs[1:]
    for count in range(min(len(candidates), len(decoder_inputs)), 0, -1):
        for start in range(len(decoder_inputs) - count + 1):
            if decoder_inputs[start : start + count] == candidates[:count]:
                mapping = {
                    f"main:#output_{index + 1}": f"arg{start + index}"
                    for index in range(count)
                }
                return mapping, candidates[:count]
    raise RuntimeError(
        f"No decoder input sequence matches the prefill cache outputs of {prefill.name}"
    )


def parse_resource_map(text: str) -> dict[str, str]:
    mapping = {}
    for entry in (text or "").split(","):
        if not entry.strip():
            continue
        name, _, tensor = entry.partition("=")
        if not tensor:
            raise RuntimeError(f"Invalid resource_map entry '{entry}'")
        mapping[name.strip()] = tensor.strip()
    return mapping


def message_text(content) -> str:
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for part in content:
            if isinstance(part, dict) and part.get("type") in ("text", "input_text"):
                parts.append(str(part.get("text", "")))
            elif isinstance(part, str):
                parts.append(part)
            else:
                raise RequestError("Only text message content is supported by AMICA")
        return "".join(parts)
    raise RequestError("Message content must be a string or a list of text parts")


class Engine:
    """The amica-engine process and its line protocol."""

    def __init__(self, command: list[str], plan: dict):
        self.process = subprocess.Popen(  # pylint: disable=consider-using-with
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self._send(plan)
        ready = self._next_event()
        if "error" in ready:
            raise RuntimeError(
                f"amica-engine failed to load the model: {ready['error']}"
            )
        self.info = ready

    def _send(self, payload: dict) -> None:
        try:
            self.process.stdin.write(json.dumps(payload) + "\n")
            self.process.stdin.flush()
        except OSError as error:
            raise EngineExited(f"amica-engine is not running: {error}") from error

    def _next_event(self) -> dict:
        while True:
            line = self.process.stdout.readline()
            if not line:
                code = self.process.wait()
                raise EngineExited(f"amica-engine exited with code {code}")
            if line.startswith(ENGINE_PREFIX):
                return json.loads(line[len(ENGINE_PREFIX) :])
            log(f"amica-engine: {line.rstrip()}")

    def generate(self, prompt_ids: list[int], max_new_tokens: int, stop_ids: set[int]):
        """Yield {"token": id} events, then the final {"done": ...} event."""
        self._send(
            {
                "prompt_ids": prompt_ids,
                "max_new_tokens": max_new_tokens,
                "stop_ids": sorted(stop_ids),
            }
        )
        while True:
            event = self._next_event()
            if "error" in event:
                raise RequestError(f"amica-engine: {event['error']}", 500)
            yield event
            if event.get("done"):
                return


class TextStream:
    """Turns generated token IDs into text deltas.

    Each delta is the growth of the decoded text, held back while the text
    ends in an incomplete UTF-8 sequence so a multi-byte character is never
    split across deltas.
    """

    def __init__(self, frontend, stop_ids: set[int]):
        self.frontend = frontend
        self.stop_ids = stop_ids
        self.ids: list[int] = []
        self.text = ""
        self.stopped = False

    def push(self, token: int) -> str:
        if token in self.stop_ids:
            self.stopped = True
            return ""
        self.ids.append(token)
        text = self.frontend.decode(self.ids)
        if text.endswith("\ufffd"):
            return ""
        delta, self.text = text[len(self.text) :], text
        return delta

    def finish(self) -> str:
        text = self.frontend.decode(self.ids) if self.ids else ""
        delta, self.text = text[len(self.text) :], text
        return delta


class PrefillGraph:
    def __init__(self, graph: Path, capacity: int, cache_map: dict[str, str]):
        self.graph = graph
        self.capacity = capacity
        self.cache_map = cache_map


class AmicaBundle:
    def __init__(
        self, model_dir: Path, tools_dir: Path, engine: Path, engine_args: list[str]
    ):
        manifest_path = model_dir / "amica_app.json"
        if not manifest_path.is_file():
            raise RuntimeError(
                f"{manifest_path} was not found. Prepare the bundle once with the "
                "AMICA ChatApp (for example Chatapp.ps1 --model <name>) to create it."
            )
        # PowerShell writes the manifest with a byte order mark.
        self.manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        self.source_dir = (
            model_dir / self.manifest.get("source_model_dir", ".")
        ).resolve()
        self.generation = self.manifest.get("generation") or {}
        if not self.generation:
            raise RuntimeError(f"{manifest_path} has no generation metadata")
        self.self_contained = self.generation.get("self_contained_logits") is True
        if self.self_contained and (
            int(self.manifest.get("schema_version", 0)) < 3
            or "zero_initialize_inputs" not in self.generation
        ):
            raise RuntimeError(
                f"{manifest_path} needs a schema-3 input plan; re-run the ChatApp "
                "preparation for this bundle"
            )
        if not self.self_contained and not self.manifest.get("decoder_append"):
            raise RuntimeError(f"{manifest_path} has no LM-head graph")
        for unsupported in ("amica_optimizations", "parameter_resources"):
            if self.manifest.get(unsupported):
                raise RuntimeError(
                    f"{manifest_path} sets {unsupported}, which the public AMICA "
                    "API used by amica-engine cannot apply"
                )
        runtime = self.manifest.get("runtime", "dx12")
        if runtime != "dx12" and "dx12" not in self.manifest.get(
            "supported_runtimes", []
        ):
            raise RuntimeError("AMICA ChatApp generation requires a DX12 bundle")

        self.decoder = self.source_dir / self.manifest["graphs"]["decoder"]
        self.prompt_padding = (
            self.generation.get("prompt_padding")
            if self.generation.get("prompt_padding") in ("left", "right")
            else "right"
        )
        self.prefill_graphs = self._load_prefill_graphs()
        self.context_capacity = self._context_capacity()

        # The tokenizer/chat-template frontend ships with the AMICA tools, not
        # with Lemonade, so it is only importable once its directory is known.
        frontend_dir = tools_dir / "chatapp" / "amdmlcompiler_app"
        if not (frontend_dir / "frontend.py").is_file():
            raise RuntimeError(f"AMICA ChatApp frontend was not found: {frontend_dir}")
        sys.path.insert(0, str(frontend_dir))
        # pylint: disable-next=import-error,import-outside-toplevel
        from frontend import ModelFrontend, _GENERATION_TAG, _token_text

        self._generation_tag = _GENERATION_TAG
        self._token_text = _token_text
        self.frontend = ModelFrontend(self.source_dir)
        self.chat_template = self.generation.get("prompt_format") == "chat_template"
        self.lock = threading.Lock()

        amica_dir = tools_dir.parent / "amica"
        if not (amica_dir / "amica.dll").is_file():
            raise RuntimeError(f"amica.dll was not found in {amica_dir}")
        self.engine = Engine(
            [str(engine), "--amica-dir", str(amica_dir), *engine_args], self.plan()
        )

    def _prefill_input(self) -> str:
        if self.self_contained:
            return self.generation["prefill_token_input"]
        return self.generation["prefill_embedding_input"]

    def _graph_capacity(self, graph: Path) -> int:
        inputs, _ = entry_point_abi(graph)
        dims = tensor_dims(inputs[parameter_index(self._prefill_input())])
        if len(dims) < 2:
            raise RuntimeError(f"Cannot determine the prefill capacity of {graph}")
        return dims[1]

    def _load_prefill_graphs(self) -> list[PrefillGraph]:
        variants = [
            variant
            for variant in self.manifest["graphs"].get("prefill_variants") or []
            if variant
        ]
        entries = []
        if variants:
            policy = (self.manifest.get("prefill_selection") or {}).get("policy")
            if policy and policy != "smallest_capacity_covering_prompt":
                raise RuntimeError(f"Unsupported prefill selection policy: {policy}")
            for variant in variants:
                entries.append(
                    (self.source_dir / variant["graph"], int(variant["capacity"]))
                )
        else:
            graph = self.source_dir / self.manifest["graphs"]["prefill"]
            entries.append((graph, self._graph_capacity(graph)))
        graphs = []
        for graph, capacity in sorted(entries, key=lambda entry: entry[1]):
            mapping, _ = cache_output_map(graph, self.decoder)
            graphs.append(PrefillGraph(graph, capacity, mapping))
        return graphs

    def _context_capacity(self) -> int | None:
        """The most tokens (prompt plus generated) one request can hold.

        A self-contained decoder takes an attention mask over the whole context,
        so its length is the limit. Otherwise the limit is the sequence length
        of the first cache tensor, when it has the [batch, heads, sequence, dim]
        layout of a KV cache.
        """
        if self.self_contained:
            decoder_inputs, _ = entry_point_abi(self.decoder)
            mask = parameter_index(self.generation["decoder_attention_mask_input"])
            elements = 1
            for dim in tensor_dims(decoder_inputs[mask]):
                elements *= dim
            return elements
        _, cache_types = cache_output_map(self.prefill_graphs[-1].graph, self.decoder)
        dims = tensor_dims(cache_types[0])
        return dims[2] if len(dims) == 4 else None

    def plan(self) -> dict:
        """Describe the bundle to amica-engine: graphs, weights and input roles."""
        generation = self.generation
        zero_initialize = [
            name.strip()
            for name in (generation.get("zero_initialize_inputs") or "").split(",")
            if name.strip()
        ]
        plan = {
            "logits": "graph" if self.self_contained else "lm_head",
            "prompt_padding": self.prompt_padding,
            "prefill": [
                {
                    "graph": str(graph.graph),
                    "capacity": graph.capacity,
                    "cache_map": graph.cache_map,
                }
                for graph in self.prefill_graphs
            ],
            "decoder": str(self.decoder),
            "resources": str(self.source_dir / self.manifest["resources"]),
            # Large tensors stay out of the compiled graphs, so prefill and
            # decoder bind one copy instead of each embedding their own.
            "external_weights_min_bytes": EXTERNAL_WEIGHTS_MIN_BYTES,
            "inputs": {
                "prefill_position": generation.get(
                    "prefill_position_input", generation.get("decoder_position_input")
                ),
                "prefill_sequence": generation.get(
                    "prefill_sequence_input", generation.get("decoder_sequence_input")
                ),
                "decoder_position": generation.get("decoder_position_input"),
                "decoder_sequence": generation.get("decoder_sequence_input"),
            },
            "zero_initialize": zero_initialize,
            "initial_input_fill": {
                name: int(value, 0)
                for name, value in parse_resource_map(
                    generation.get("initial_input_fill")
                ).items()
            },
        }
        if self.self_contained:
            plan["vocab_size"] = int(generation["vocab_size"])
            plan["pad_token_id"] = int(generation.get("pad_token_id") or 0)
            plan["inputs"].update(
                {
                    "prefill_token": generation["prefill_token_input"],
                    "prefill_attention_mask": generation[
                        "prefill_attention_mask_input"
                    ],
                    "decoder_token": generation["decoder_token_input"],
                    "decoder_attention_mask": generation[
                        "decoder_attention_mask_input"
                    ],
                }
            )
            return plan

        tail = self.manifest["decoder_append"]
        plan["lm_head"] = {
            "graph": str(self.source_dir / tail["graph"]),
            "resources": str(self.source_dir / tail["resources"]),
            "resource_map": parse_resource_map(tail.get("resource_map")),
        }
        # amica-driver binds the LM head through the decoder's embedding
        # parameter name; the manifests rely on that convention.
        plan["tail_input"] = generation["decoder_embedding_input"]
        plan["tail_row_offset"] = int(generation.get("prefill_tail_row_offset") or 0)
        plan["embedding"] = {
            "file": str(self.source_dir / generation["embedding_file"]),
            "tensor": generation["embedding_tensor"],
        }
        plan["inputs"].update(
            {
                "prefill_embedding": generation["prefill_embedding_input"],
                "decoder_embedding": generation["decoder_embedding_input"],
            }
        )
        return plan

    @property
    def max_prompt_tokens(self) -> int:
        return self.prefill_graphs[-1].capacity

    def stop_token_ids(self, chat: bool) -> set[int]:
        if chat and self.chat_template:
            return self.frontend.chat_stop_token_ids()
        return set(self.frontend.eos_token_ids)

    def render_messages(self, messages: list[dict]) -> str:
        if not self.chat_template:
            return "\n".join(message["content"] for message in messages)
        frontend = self.frontend
        template_text = frontend.tokenizer_config.get("chat_template")
        template_path = frontend.model_dir / "chat_template.jinja"
        if not template_text and template_path.is_file():
            template_text = template_path.read_text(encoding="utf-8")
        if not isinstance(template_text, str) or not template_text:
            system = next(
                (m["content"] for m in messages if m["role"] == "system"), None
            )
            user = next(
                (m["content"] for m in reversed(messages) if m["role"] == "user"), ""
            )
            return frontend.render_chat(user, system)

        # pylint: disable-next=import-error,import-outside-toplevel
        import jinja2

        def raise_exception(message):
            raise jinja2.exceptions.TemplateError(message)

        environment = jinja2.Environment(trim_blocks=False, lstrip_blocks=False)
        environment.globals["raise_exception"] = raise_exception
        environment.globals["strftime_now"] = datetime.datetime.now().strftime
        template = environment.from_string(self._generation_tag.sub("", template_text))
        return template.render(
            messages=messages,
            add_generation_prompt=True,
            bos_token=self._token_text(frontend.tokenizer_config.get("bos_token"))
            or "",
            eos_token=self._token_text(frontend.tokenizer_config.get("eos_token"))
            or "",
        )

    def encode_messages(self, messages: list[dict]) -> list[int]:
        ids = self.frontend.encode(self.render_messages(messages))
        if self.chat_template and self.frontend.tokenizer_config.get(
            "add_bos_token", False
        ):
            bos = self.frontend.generation_config.get(
                "bos_token_id", self.frontend.tokenizer_config.get("bos_token_id")
            )
            if isinstance(bos, list):
                bos = bos[0] if bos else None
            if bos is not None and (not ids or ids[0] != int(bos)):
                ids.insert(0, int(bos))
        return ids

    def fit_messages(self, messages: list[dict]) -> list[int]:
        """Tokenize the conversation, dropping the oldest turns to fit prefill."""
        messages = list(messages)
        while True:
            ids = self.encode_messages(messages)
            if len(ids) <= self.max_prompt_tokens:
                return ids
            droppable = [
                index
                for index, message in enumerate(messages)
                if message["role"] != "system"
            ]
            if len(droppable) <= 1:
                raise RequestError(
                    f"The prompt is {len(ids)} tokens, but this AMICA bundle "
                    f"accepts at most {self.max_prompt_tokens} prompt tokens"
                )
            del messages[droppable[0]]

    def token_budget(self, prompt_tokens: int, requested: int | None) -> int:
        limit = (
            self.context_capacity - prompt_tokens
            if self.context_capacity is not None
            else DEFAULT_MAX_NEW_TOKENS
        )
        if limit < 1:
            raise RequestError(
                f"The prompt fills this AMICA bundle's {self.context_capacity}-token context"
            )
        if requested is None:
            return limit
        if requested < 1:
            raise RequestError("max_tokens must be at least 1")
        return min(requested, limit)

    def complete(self, prompt_ids: list[int], requested: int | None, chat: bool):
        """Yield text deltas, then (finish_reason, usage, llama.cpp timings)."""
        stop_ids = self.stop_token_ids(chat)
        max_new_tokens = self.token_budget(len(prompt_ids), requested)
        stream = TextStream(self.frontend, stop_ids)
        started = time.perf_counter()
        done = {}
        with self.lock:
            for event in self.engine.generate(prompt_ids, max_new_tokens, stop_ids):
                if "token" in event:
                    delta = stream.push(int(event["token"]))
                    if delta:
                        yield delta
                done = event
        delta = stream.finish()
        if delta:
            yield delta

        completion_tokens = len(done["ids"])
        finish_reason = "stop" if stream.stopped else "length"
        timings = {
            "prompt_n": len(prompt_ids),
            "prompt_ms": done["ttft_ms"],
            "predicted_n": completion_tokens,
            "predicted_ms": done["decode_ms"],
        }
        if done["tps"]:
            timings["predicted_per_second"] = done["tps"]
        log(
            f"prompt_tokens={len(prompt_ids)} generated_tokens={completion_tokens} "
            f"finish={finish_reason} ttft_ms={done['ttft_ms']:.1f} "
            f"tps={done['tps']:.1f} "
            f"request_ms={(time.perf_counter() - started) * 1000.0:.0f}"
        )
        usage = {
            "prompt_tokens": len(prompt_ids),
            "completion_tokens": completion_tokens,
            "total_tokens": len(prompt_ids) + completion_tokens,
        }
        yield finish_reason, usage, timings


def requested_max_tokens(body: dict) -> int | None:
    for key in ("max_completion_tokens", "max_tokens"):
        value = body.get(key)
        if value is not None:
            if not isinstance(value, int) or isinstance(value, bool):
                raise RequestError(f"'{key}' must be an integer")
            return value
    return None


def chat_messages(body: dict) -> list[dict]:
    messages = body.get("messages")
    if not isinstance(messages, list) or not messages:
        raise RequestError("'messages' must be a non-empty array")
    result = []
    for message in messages:
        if not isinstance(message, dict):
            raise RequestError("Each message must be an object")
        role = message.get("role")
        if role == "developer":
            role = "system"
        if role not in ("system", "user", "assistant"):
            continue
        result.append({"role": role, "content": message_text(message.get("content"))})
    if not any(message["role"] == "user" for message in result):
        raise RequestError("The conversation must contain a user message")
    return result


def exit_after_engine_failure() -> None:
    """Exit shortly after the error response so Lemonade reloads the model."""

    def stop():
        time.sleep(0.5)
        os._exit(1)

    threading.Thread(target=stop, daemon=True).start()


class AmicaHTTPServer(ThreadingHTTPServer):
    def __init__(self, address, bundle: AmicaBundle, model_name: str):
        super().__init__(address, Handler)
        self.bundle = bundle
        self.model_name = model_name


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def bundle(self) -> AmicaBundle:
        return self.server.bundle

    @property
    def model_name(self) -> str:
        return self.server.model_name

    def log_message(self, format, *args):  # pylint: disable=redefined-builtin
        if self.path != "/health":
            log(format % args)

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, error: RequestError) -> None:
        error_type = "invalid_request_error" if error.status < 500 else "server_error"
        self._send_json(
            error.status, {"error": {"message": str(error), "type": error_type}}
        )

    def _start_stream(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True  # pylint: disable=attribute-defined-outside-init

    def _send_event(self, payload) -> None:
        data = payload if isinstance(payload, str) else json.dumps(payload)
        self.wfile.write(f"data: {data}\n\n".encode("utf-8"))
        self.wfile.flush()

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {"status": "ok"})
        elif self.path in ("/v1/models", "/models"):
            self._send_json(
                200,
                {
                    "object": "list",
                    "data": [
                        {"id": self.model_name, "object": "model", "owned_by": "amica"}
                    ],
                },
            )
        else:
            self._send_json(404, {"error": {"message": "Not found"}})

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(body, dict):
                raise RequestError("The request body must be a JSON object")
            if self.path in ("/v1/chat/completions", "/chat/completions"):
                self._chat_completion(body)
            elif self.path in ("/v1/completions", "/completions"):
                self._completion(body)
            else:
                self._send_json(404, {"error": {"message": "Not found"}})
        except json.JSONDecodeError as error:
            self._send_error(RequestError(f"Invalid JSON: {error}"))
        except EngineExited as error:
            log(str(error))
            self._send_error(error)
            exit_after_engine_failure()
        except RequestError as error:
            self._send_error(error)
        except Exception as error:  # pylint: disable=broad-exception-caught
            log(f"request failed: {error}")
            self._send_error(RequestError(str(error), 500))

    def _respond(self, results, stream: bool, chunk) -> None:
        """Send a completion, streamed as server-sent events or as one JSON body.

        chunk(text, finish_reason) builds the response object; in streaming mode
        it carries one delta, otherwise the whole text.
        """
        if not stream:
            *parts, (finish_reason, usage, timings) = list(results)
            response = chunk("".join(parts), finish_reason)
            response["usage"] = usage
            response["timings"] = timings
            self._send_json(200, response)
            return

        self._start_stream()
        connected = True
        try:
            for item in results:
                if not connected:
                    continue
                try:
                    if isinstance(item, str):
                        self._send_event(chunk(item, None))
                    else:
                        finish_reason, usage, timings = item
                        final = chunk(None, finish_reason)
                        final["usage"] = usage
                        final["timings"] = timings
                        self._send_event(final)
                        self._send_event("[DONE]")
                except OSError:
                    # Keep draining so the engine finishes this request and the
                    # line protocol stays in step for the next one.
                    connected = False
        except RequestError as error:
            if isinstance(error, EngineExited):
                exit_after_engine_failure()
            if connected:
                self._send_event({"error": {"message": str(error)}})

    def _chat_completion(self, body: dict) -> None:
        prompt_ids = self.bundle.fit_messages(chat_messages(body))
        requested = requested_max_tokens(body)
        completion_id = f"chatcmpl-{uuid.uuid4().hex}"
        created = int(time.time())
        stream = bool(body.get("stream"))
        first = [True]

        def chunk(text, finish_reason) -> dict:
            if not stream:
                return {
                    "id": completion_id,
                    "object": "chat.completion",
                    "created": created,
                    "model": self.model_name,
                    "choices": [
                        {
                            "index": 0,
                            "message": {"role": "assistant", "content": text},
                            "finish_reason": finish_reason,
                        }
                    ],
                }
            delta = {} if text is None else {"content": text}
            if first[0] and text is not None:
                delta["role"] = "assistant"
                first[0] = False
            return {
                "id": completion_id,
                "object": "chat.completion.chunk",
                "created": created,
                "model": self.model_name,
                "choices": [
                    {"index": 0, "delta": delta, "finish_reason": finish_reason}
                ],
            }

        self._respond(
            self.bundle.complete(prompt_ids, requested, chat=True), stream, chunk
        )

    def _completion(self, body: dict) -> None:
        prompt = body.get("prompt")
        if isinstance(prompt, list) and len(prompt) == 1:
            prompt = prompt[0]
        if not isinstance(prompt, str) or not prompt:
            raise RequestError("'prompt' must be a non-empty string")
        prompt_ids = self.bundle.frontend.encode(prompt)
        if len(prompt_ids) > self.bundle.max_prompt_tokens:
            raise RequestError(
                f"The prompt is {len(prompt_ids)} tokens, but this AMICA bundle "
                f"accepts at most {self.bundle.max_prompt_tokens} prompt tokens"
            )
        requested = requested_max_tokens(body)
        completion_id = f"cmpl-{uuid.uuid4().hex}"
        created = int(time.time())

        def chunk(text, finish_reason) -> dict:
            return {
                "id": completion_id,
                "object": "text_completion",
                "created": created,
                "model": self.model_name,
                "choices": [
                    {"index": 0, "text": text or "", "finish_reason": finish_reason}
                ],
            }

        self._respond(
            self.bundle.complete(prompt_ids, requested, chat=False),
            bool(body.get("stream")),
            chunk,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", required=True, type=Path)
    parser.add_argument(
        "--tools-dir",
        required=True,
        type=Path,
        help="AMICA install tools directory (holds chatapp/ beside ../amica/)",
    )
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--model-name", default="amica")
    parser.add_argument(
        "--engine-arg",
        action="append",
        default=[],
        help="Extra amica-engine argument; use --engine-arg=VALUE for dashed values",
    )
    args = parser.parse_args()

    bundle = AmicaBundle(
        args.model_dir.resolve(),
        args.tools_dir.resolve(),
        args.engine.resolve(),
        args.engine_arg,
    )
    info = bundle.engine.info
    log(
        f"model={args.model_dir} adapter='{info.get('adapter')}' arch={info.get('arch')} "
        f"prefill_capacity={info.get('prefill_capacities')} "
        f"context={bundle.context_capacity} padding={bundle.prompt_padding} "
        f"load_ms={info.get('load_ms', 0):.0f}"
    )
    server = AmicaHTTPServer((args.host, args.port), bundle, args.model_name)
    log(f"listening on http://{args.host}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
