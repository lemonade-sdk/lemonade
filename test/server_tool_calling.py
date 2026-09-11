"""
Tool-calling test battery.

Exercises tool calling end to end through every API surface Lemonade
translates (OpenAI chat, Responses, Anthropic messages, Ollama chat) against
the backend's dedicated tool_calling test model.

Usage:
    python test/server_tool_calling.py --wrapped-server llamacpp --backend vulkan
    python test/server_tool_calling.py --wrapped-server flm --backend npu
"""

import json
import re
from concurrent.futures import ThreadPoolExecutor

import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
    _auth_headers,
)
from utils.capabilities import skip_if_unsupported
from utils.test_models import (
    PORT,
    SAMPLE_TOOL,
    TIMEOUT_MODEL_OPERATION,
)

SERVER_URL = f"http://localhost:{PORT}"

WEATHER_TOOL = {
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "Get the current weather for a city",
        "parameters": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"},
                "unit": {"type": "string", "enum": ["celsius", "fahrenheit"]},
            },
            "required": ["city"],
        },
    },
}

ECHO_TOOL = {
    "type": "function",
    "function": {
        "name": "echo_text",
        "description": "Echo the given text back verbatim",
        "parameters": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
}

TYPED_TOOL = {
    "type": "function",
    "function": {
        "name": "set_flag",
        "description": "Set a named feature flag",
        "parameters": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "enabled": {"type": "boolean"},
                "count": {"type": "integer"},
            },
            "required": ["name", "enabled", "count"],
        },
    },
}

NO_ARGS_TOOL = {
    "type": "function",
    "function": {
        "name": "get_current_time",
        "description": "Get the current time. Takes no arguments.",
        "parameters": {"type": "object", "properties": {}},
    },
}

CALC_PROMPT = "Run the calculator_calculate tool with expression set to 1+1"
WEATHER_PROMPT = "Use the get_weather tool to check the weather in Paris."
WEATHER_RESULT = '{"temperature_c": 19, "condition": "cloudy"}'
DETERMINISTIC = {"temperature": 0, "top_p": 1, "seed": 42}


def _normalize_expression(expr):
    return re.sub(r"\s+", "", str(expr))


def _assemble_streamed_tool_calls(stream):
    """Merge OpenAI streaming tool_call deltas by index; return (calls, finish_reasons, saw_index)."""
    calls = {}
    finish_reasons = []
    saw_index = True
    for chunk in stream:
        if not chunk.choices:
            continue
        choice = chunk.choices[0]
        if choice.finish_reason:
            finish_reasons.append(choice.finish_reason)
        delta = choice.delta
        if not delta or not delta.tool_calls:
            continue
        for tc in delta.tool_calls:
            if tc.index is None:
                saw_index = False
            idx = tc.index if tc.index is not None else 0
            entry = calls.setdefault(idx, {"id": None, "name": "", "arguments": ""})
            if tc.id:
                entry["id"] = tc.id
            if tc.function:
                if tc.function.name:
                    entry["name"] += tc.function.name
                if tc.function.arguments:
                    entry["arguments"] += tc.function.arguments
    return [calls[k] for k in sorted(calls)], finish_reasons, saw_index


def _parse_sse(response):
    events = []
    for raw in response.iter_lines():
        if not raw:
            continue
        line = raw.decode("utf-8")
        if line.startswith("data:"):
            payload = line[5:].strip()
            if payload and payload != "[DONE]":
                events.append(json.loads(payload))
    return events


class ToolCallingTests(ServerTestBase):
    """Tool-calling behaviour through every API surface."""

    def setUp(self):
        super().setUp()
        self.model = self.get_test_model("tool_calling")
        self.headers = {"Content-Type": "application/json", **_auth_headers()}

    def _chat(self, **kwargs):
        client = self.get_openai_client()
        params = dict(model=self.model, max_completion_tokens=200, **DETERMINISTIC)
        params.update(kwargs)
        return client.chat.completions.create(**params)

    def _weather_round_trip_messages(self):
        """Run a weather tool call and return the messages for the follow-up turn."""
        first = self._chat(
            messages=[{"role": "user", "content": WEATHER_PROMPT}],
            tools=[WEATHER_TOOL],
        )
        tool_calls = first.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls, "Response should have tool_calls")
        self.assertEqual(len(tool_calls), 1)
        call = tool_calls[0]
        self.assertEqual(call.function.name, "get_weather")
        return [
            {"role": "user", "content": WEATHER_PROMPT},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "id": call.id,
                        "type": "function",
                        "function": {
                            "name": call.function.name,
                            "arguments": call.function.arguments,
                        },
                    }
                ],
            },
            {"role": "tool", "tool_call_id": call.id, "content": WEATHER_RESULT},
        ]

    def _assert_single_calc_call(self, tool_calls, expected="1+1"):
        self.assertIsNotNone(tool_calls, "Response should have tool_calls")
        self.assertEqual(len(tool_calls), 1)
        call = tool_calls[0]
        self.assertEqual(call.type, "function")
        self.assertTrue(call.id, "tool call must carry an id")
        self.assertEqual(call.function.name, "calculator_calculate")
        args = json.loads(call.function.arguments)
        self.assertIn("expression", args)
        self.assertEqual(_normalize_expression(args["expression"]), expected)
        return call

    # ------------------------------------------------------------------
    # OpenAI chat completions
    # ------------------------------------------------------------------

    @skip_if_unsupported("tool_calls")
    def test_001_single_tool_call(self):
        """Basic tool call: name, parseable arguments, id, finish_reason."""
        completion = self._chat(
            messages=[{"role": "user", "content": CALC_PROMPT}],
            tools=[SAMPLE_TOOL],
        )
        choice = completion.choices[0]
        self._assert_single_calc_call(choice.message.tool_calls)
        self.assertEqual(choice.finish_reason, "tool_calls")
        self.assertFalse(
            (choice.message.content or "").strip(),
            f"content should be empty alongside a tool call, got: {choice.message.content!r}",
        )
        self.assertGreater(completion.usage.completion_tokens, 0)

    @skip_if_unsupported("tool_calls_streaming")
    def test_002_single_tool_call_streaming(self):
        """Streaming deltas reassemble into the same tool call."""
        stream = self._chat(
            messages=[{"role": "user", "content": CALC_PROMPT}],
            tools=[SAMPLE_TOOL],
            stream=True,
        )
        calls, finish_reasons, saw_index = _assemble_streamed_tool_calls(stream)
        self.assertTrue(saw_index, "every streamed tool_call delta must carry an index")
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["id"], "streamed tool call must carry an id")
        self.assertEqual(calls[0]["name"], "calculator_calculate")
        args = json.loads(calls[0]["arguments"])
        self.assertEqual(_normalize_expression(args["expression"]), "1+1")
        self.assertEqual(finish_reasons, ["tool_calls"])

    @skip_if_unsupported("tool_calls")
    def test_003_tool_result_round_trip(self):
        """Feeding the tool result back yields a text answer that uses it."""
        messages = self._weather_round_trip_messages()
        second = self._chat(messages=messages, tools=[WEATHER_TOOL])
        choice = second.choices[0]
        self.assertIsNone(choice.message.tool_calls, "second turn should be text")
        self.assertEqual(choice.finish_reason, "stop")
        self.assertIn("19", choice.message.content)

    @skip_if_unsupported("tool_calls_streaming")
    def test_004_tool_result_round_trip_streaming(self):
        """Streaming the post-tool turn yields text deltas and no tool_calls."""
        messages = self._weather_round_trip_messages()
        stream = self._chat(messages=messages, tools=[WEATHER_TOOL], stream=True)
        text = ""
        tool_deltas = 0
        finish_reasons = []
        for chunk in stream:
            if not chunk.choices:
                continue
            choice = chunk.choices[0]
            if choice.finish_reason:
                finish_reasons.append(choice.finish_reason)
            if choice.delta and choice.delta.content:
                text += choice.delta.content
            if choice.delta and choice.delta.tool_calls:
                tool_deltas += 1
        self.assertEqual(tool_deltas, 0)
        self.assertEqual(finish_reasons, ["stop"])
        self.assertIn("19", text)

    @skip_if_unsupported("tool_calls")
    def test_005_two_sequential_tool_rounds(self):
        """A second tool request after a completed tool round produces a second call."""
        first = self._chat(
            messages=[{"role": "user", "content": CALC_PROMPT}],
            tools=[SAMPLE_TOOL],
        )
        call = self._assert_single_calc_call(first.choices[0].message.tool_calls)
        messages = [
            {"role": "user", "content": CALC_PROMPT},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "id": call.id,
                        "type": "function",
                        "function": {
                            "name": call.function.name,
                            "arguments": call.function.arguments,
                        },
                    }
                ],
            },
            {"role": "tool", "tool_call_id": call.id, "content": "2"},
            {
                "role": "user",
                "content": "Now run the calculator_calculate tool with expression set to 3*4",
            },
        ]
        second = self._chat(messages=messages, tools=[SAMPLE_TOOL])
        self._assert_single_calc_call(second.choices[0].message.tool_calls, "3*4")

    @skip_if_unsupported("tool_calls")
    def test_006_selects_correct_tool(self):
        """With several tools offered, the model picks the one that matches the request."""
        completion = self._chat(
            messages=[
                {
                    "role": "user",
                    "content": "Use the get_weather tool to check the weather in Paris.",
                }
            ],
            tools=[SAMPLE_TOOL, WEATHER_TOOL, ECHO_TOOL],
        )
        tool_calls = completion.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(len(tool_calls), 1)
        self.assertEqual(tool_calls[0].function.name, "get_weather")
        args = json.loads(tool_calls[0].function.arguments)
        self.assertIn("paris", args["city"].lower())

    @skip_if_unsupported("tool_calls")
    def test_007_no_tool_when_not_needed(self):
        """Tools offered but unnecessary: plain text, no tool_calls."""
        completion = self._chat(
            messages=[
                {"role": "user", "content": "Reply with exactly the word hello."}
            ],
            tools=[SAMPLE_TOOL, WEATHER_TOOL],
            max_completion_tokens=20,
        )
        choice = completion.choices[0]
        self.assertIsNone(choice.message.tool_calls)
        self.assertIn("hello", (choice.message.content or "").lower())
        self.assertEqual(choice.finish_reason, "stop")

    @skip_if_unsupported("tool_choice")
    def test_008_tool_choice_none(self):
        """tool_choice=none suppresses tool calls even when asked for one."""
        completion = self._chat(
            messages=[{"role": "user", "content": CALC_PROMPT}],
            tools=[SAMPLE_TOOL],
            tool_choice="none",
            max_completion_tokens=60,
        )
        choice = completion.choices[0]
        self.assertIsNone(choice.message.tool_calls)
        self.assertTrue((choice.message.content or "").strip())

    @skip_if_unsupported("tool_choice")
    def test_009_tool_choice_required(self):
        """tool_choice=required forces a call for a prompt that would otherwise be text."""
        completion = self._chat(
            messages=[
                {"role": "user", "content": "What is the weather in Tokyo today?"}
            ],
            tools=[WEATHER_TOOL],
            tool_choice="required",
        )
        choice = completion.choices[0]
        self.assertIsNotNone(choice.message.tool_calls)
        self.assertEqual(choice.message.tool_calls[0].function.name, "get_weather")
        self.assertEqual(choice.finish_reason, "tool_calls")

    @skip_if_unsupported("tool_choice")
    def test_010_tool_choice_named_function(self):
        """tool_choice naming a function routes to that function."""
        completion = self._chat(
            messages=[
                {"role": "user", "content": "Tell me about the weather in Tokyo."}
            ],
            tools=[SAMPLE_TOOL, WEATHER_TOOL],
            tool_choice={"type": "function", "function": {"name": "get_weather"}},
        )
        tool_calls = completion.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(tool_calls[0].function.name, "get_weather")
        self.assertIn(
            "tokyo", json.loads(tool_calls[0].function.arguments)["city"].lower()
        )

    @skip_if_unsupported("tool_calls")
    def test_011_typed_arguments(self):
        """Boolean and integer arguments arrive as JSON booleans and integers."""
        completion = self._chat(
            messages=[
                {
                    "role": "user",
                    "content": "Call set_flag with name dark_mode, enabled true, and count 3.",
                }
            ],
            tools=[TYPED_TOOL],
        )
        tool_calls = completion.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(tool_calls[0].function.name, "set_flag")
        args = json.loads(tool_calls[0].function.arguments)
        self.assertEqual(args["name"], "dark_mode")
        self.assertIs(args["enabled"], True)
        self.assertIsInstance(args["count"], int)
        self.assertEqual(args["count"], 3)

    @skip_if_unsupported("tool_calls")
    def test_012_argument_escaping(self):
        """Quotes and non-ASCII text survive JSON encoding of arguments."""
        text = 'Say "hi" to the café'
        completion = self._chat(
            messages=[
                {
                    "role": "user",
                    "content": f"Call echo_text with text set to exactly: {text}",
                }
            ],
            tools=[ECHO_TOOL],
        )
        tool_calls = completion.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(tool_calls[0].function.name, "echo_text")
        args = json.loads(tool_calls[0].function.arguments)
        self.assertIn('"hi"', args["text"])
        self.assertIn("café", args["text"])

    @skip_if_unsupported("tool_calls")
    def test_013_parallel_tool_calls(self):
        """Two independent requests in one turn yield two tool calls."""
        completion = self._chat(
            messages=[
                {
                    "role": "user",
                    "content": (
                        "Call calculator_calculate twice in parallel: once with expression 2+2 "
                        "and once with expression 3+3."
                    ),
                }
            ],
            tools=[SAMPLE_TOOL],
        )
        tool_calls = completion.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(len(tool_calls), 2)
        exprs = sorted(
            _normalize_expression(json.loads(tc.function.arguments)["expression"])
            for tc in tool_calls
        )
        self.assertEqual(exprs, ["2+2", "3+3"])
        self.assertEqual(len({tc.id for tc in tool_calls}), 2, "ids must be unique")

    @skip_if_unsupported("tool_calls")
    def test_014_tool_call_with_system_and_history(self):
        """A system prompt and prior turns do not break tool selection."""
        completion = self._chat(
            messages=[
                {
                    "role": "system",
                    "content": "You are a precise assistant. Use tools when asked.",
                },
                {"role": "user", "content": "Hi there."},
                {"role": "assistant", "content": "Hello! How can I help?"},
                {"role": "user", "content": CALC_PROMPT},
            ],
            tools=[SAMPLE_TOOL],
        )
        self._assert_single_calc_call(completion.choices[0].message.tool_calls)

    @skip_if_unsupported("tool_calls")
    def test_015_no_tools_no_tool_calls(self):
        """Without tools in the request, the response has no tool_calls."""
        completion = self._chat(
            messages=[
                {"role": "user", "content": "What is 1+1? Answer with just the number."}
            ],
            max_completion_tokens=20,
        )
        choice = completion.choices[0]
        self.assertIsNone(choice.message.tool_calls)
        self.assertIn("2", choice.message.content)

    @skip_if_unsupported("tool_calls_streaming")
    def test_016_parallel_tool_calls_streaming(self):
        """Streamed parallel calls arrive under distinct indices with distinct ids."""
        stream = self._chat(
            messages=[
                {
                    "role": "user",
                    "content": (
                        "Call get_weather twice in parallel: once for Paris and once for Tokyo."
                    ),
                }
            ],
            tools=[WEATHER_TOOL],
            stream=True,
        )
        calls, finish_reasons, saw_index = _assemble_streamed_tool_calls(stream)
        self.assertTrue(saw_index)
        self.assertEqual(len(calls), 2, calls)
        self.assertEqual(len({c["id"] for c in calls}), 2, "ids must be unique")
        cities = sorted(json.loads(c["arguments"])["city"].lower() for c in calls)
        self.assertEqual(cities, ["paris", "tokyo"])
        self.assertEqual(finish_reasons, ["tool_calls"])

    @skip_if_unsupported("tool_calls")
    def test_017_multiple_tool_results_in_one_turn(self):
        """Two tool results fed back in one turn are both reflected in the answer."""
        prompt = (
            "Call get_weather twice in parallel: once for Paris and once for Tokyo."
        )
        first = self._chat(
            messages=[{"role": "user", "content": prompt}], tools=[WEATHER_TOOL]
        )
        tool_calls = first.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(len(tool_calls), 2)
        results = {"paris": "19", "tokyo": "27"}
        messages = [
            {"role": "user", "content": prompt},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "id": tc.id,
                        "type": "function",
                        "function": {
                            "name": tc.function.name,
                            "arguments": tc.function.arguments,
                        },
                    }
                    for tc in tool_calls
                ],
            },
        ]
        for tc in tool_calls:
            city = json.loads(tc.function.arguments)["city"].lower()
            temp = next(v for k, v in results.items() if k in city)
            messages.append(
                {
                    "role": "tool",
                    "tool_call_id": tc.id,
                    "content": json.dumps(
                        {"temperature_c": int(temp), "condition": "clear"}
                    ),
                }
            )
        second = self._chat(messages=messages, tools=[WEATHER_TOOL])
        choice = second.choices[0]
        self.assertIsNone(choice.message.tool_calls)
        self.assertIn("19", choice.message.content)
        self.assertIn("27", choice.message.content)

    @skip_if_unsupported("tool_calls")
    def test_018_concurrent_tool_call_requests(self):
        """Simultaneous tool-call requests each get a complete, correct tool call."""
        exprs = ["1+1", "2*3", "10-4"]

        def run(expr):
            completion = self._chat(
                messages=[
                    {
                        "role": "user",
                        "content": f"Run the calculator_calculate tool with expression set to {expr}",
                    }
                ],
                tools=[SAMPLE_TOOL],
            )
            return completion.choices[0].message.tool_calls

        with ThreadPoolExecutor(max_workers=len(exprs)) as pool:
            results = list(pool.map(run, exprs))
        for expr, tool_calls in zip(exprs, results):
            self._assert_single_calc_call(tool_calls, expr)

    @skip_if_unsupported("tool_calls")
    def test_019_tool_without_parameters(self):
        """A parameterless tool yields an empty JSON object for arguments."""
        completion = self._chat(
            messages=[{"role": "user", "content": "Use the get_current_time tool."}],
            tools=[NO_ARGS_TOOL],
        )
        tool_calls = completion.choices[0].message.tool_calls
        self.assertIsNotNone(tool_calls)
        self.assertEqual(tool_calls[0].function.name, "get_current_time")
        args = json.loads(tool_calls[0].function.arguments or "{}")
        self.assertEqual(args, {})

    # ------------------------------------------------------------------
    # Responses API
    # ------------------------------------------------------------------

    @skip_if_unsupported("responses_api")
    def test_020_responses_api_function_call(self):
        """Responses API emits a function_call output item."""
        client = self.get_openai_client()
        response = client.responses.create(
            model=self.model,
            input=CALC_PROMPT,
            tools=[
                {
                    "type": "function",
                    "name": "calculator_calculate",
                    "parameters": SAMPLE_TOOL["function"]["parameters"],
                }
            ],
            temperature=0,
            max_output_tokens=200,
        )
        calls = [item for item in response.output if item.type == "function_call"]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0].name, "calculator_calculate")
        args = json.loads(calls[0].arguments)
        self.assertEqual(_normalize_expression(args["expression"]), "1+1")

    # ------------------------------------------------------------------
    # Anthropic messages API
    # ------------------------------------------------------------------

    def _anthropic_tool(self):
        return {
            "name": SAMPLE_TOOL["function"]["name"],
            "description": "Evaluate a math expression",
            "input_schema": SAMPLE_TOOL["function"]["parameters"],
        }

    @skip_if_unsupported("tool_calls")
    def test_030_anthropic_tool_use(self):
        """Anthropic /v1/messages returns a tool_use block with parsed input."""
        payload = {
            "model": self.model,
            "messages": [{"role": "user", "content": CALC_PROMPT}],
            "tools": [self._anthropic_tool()],
            "max_tokens": 200,
            "temperature": 0,
        }
        response = requests.post(
            f"{SERVER_URL}/v1/messages",
            json=payload,
            headers=self.headers,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200, response.text)
        data = response.json()
        blocks = [b for b in data["content"] if b.get("type") == "tool_use"]
        self.assertEqual(len(blocks), 1, data)
        self.assertEqual(blocks[0]["name"], "calculator_calculate")
        self.assertTrue(blocks[0].get("id"))
        self.assertEqual(_normalize_expression(blocks[0]["input"]["expression"]), "1+1")
        self.assertEqual(data.get("stop_reason"), "tool_use")

    @skip_if_unsupported("tool_calls_streaming")
    def test_031_anthropic_tool_use_streaming(self):
        """Anthropic streaming emits tool_use block start, input_json_delta, and stop_reason."""
        payload = {
            "model": self.model,
            "messages": [{"role": "user", "content": CALC_PROMPT}],
            "tools": [self._anthropic_tool()],
            "max_tokens": 200,
            "temperature": 0,
            "stream": True,
        }
        response = requests.post(
            f"{SERVER_URL}/v1/messages",
            json=payload,
            headers=self.headers,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200, response.text)
        events = _parse_sse(response)
        starts = [
            e
            for e in events
            if e.get("type") == "content_block_start"
            and e.get("content_block", {}).get("type") == "tool_use"
        ]
        self.assertEqual(len(starts), 1, [e.get("type") for e in events])
        self.assertEqual(starts[0]["content_block"]["name"], "calculator_calculate")
        partial = "".join(
            e["delta"].get("partial_json", "")
            for e in events
            if e.get("type") == "content_block_delta"
            and e.get("delta", {}).get("type") == "input_json_delta"
        )
        self.assertEqual(
            _normalize_expression(json.loads(partial)["expression"]), "1+1"
        )
        stop_reasons = [
            e["delta"].get("stop_reason")
            for e in events
            if e.get("type") == "message_delta"
        ]
        self.assertIn("tool_use", stop_reasons)

    @skip_if_unsupported("tool_calls")
    def test_032_anthropic_tool_result_round_trip(self):
        """Anthropic tool_result blocks feed back into a text answer."""
        weather_tool = {
            "name": "get_weather",
            "description": WEATHER_TOOL["function"]["description"],
            "input_schema": WEATHER_TOOL["function"]["parameters"],
        }
        first = requests.post(
            f"{SERVER_URL}/v1/messages",
            json={
                "model": self.model,
                "messages": [{"role": "user", "content": WEATHER_PROMPT}],
                "tools": [weather_tool],
                "max_tokens": 200,
                "temperature": 0,
            },
            headers=self.headers,
            timeout=TIMEOUT_MODEL_OPERATION,
        ).json()
        block = next(b for b in first["content"] if b.get("type") == "tool_use")
        payload = {
            "model": self.model,
            "messages": [
                {"role": "user", "content": WEATHER_PROMPT},
                {"role": "assistant", "content": first["content"]},
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "tool_result",
                            "tool_use_id": block["id"],
                            "content": WEATHER_RESULT,
                        }
                    ],
                },
            ],
            "tools": [weather_tool],
            "max_tokens": 200,
            "temperature": 0,
        }
        response = requests.post(
            f"{SERVER_URL}/v1/messages",
            json=payload,
            headers=self.headers,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200, response.text)
        data = response.json()
        text = "".join(
            b.get("text", "") for b in data["content"] if b.get("type") == "text"
        )
        self.assertIn("19", text)
        self.assertEqual(data.get("stop_reason"), "end_turn")

    # ------------------------------------------------------------------
    # Ollama chat API
    # ------------------------------------------------------------------

    @skip_if_unsupported("tool_calls")
    def test_040_ollama_chat_tool_call(self):
        """Ollama /api/chat returns message.tool_calls with a dict of arguments."""
        response = requests.post(
            f"{SERVER_URL}/api/chat",
            json={
                "model": self.model,
                "messages": [{"role": "user", "content": CALC_PROMPT}],
                "tools": [SAMPLE_TOOL],
                "stream": False,
                "options": {"num_predict": 200, "temperature": 0},
            },
            headers=self.headers,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200, response.text)
        data = response.json()
        tool_calls = data["message"].get("tool_calls")
        self.assertTrue(tool_calls, data)
        self.assertEqual(tool_calls[0]["function"]["name"], "calculator_calculate")
        args = tool_calls[0]["function"]["arguments"]
        self.assertIsInstance(args, dict)
        self.assertEqual(_normalize_expression(args["expression"]), "1+1")

    @skip_if_unsupported("tool_calls_streaming")
    def test_041_ollama_chat_tool_call_streaming(self):
        """Ollama streaming /api/chat carries the tool call in a chunk and ends with done."""
        response = requests.post(
            f"{SERVER_URL}/api/chat",
            json={
                "model": self.model,
                "messages": [{"role": "user", "content": CALC_PROMPT}],
                "tools": [SAMPLE_TOOL],
                "stream": True,
                "options": {"num_predict": 200, "temperature": 0},
            },
            headers=self.headers,
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200, response.text)
        chunks = [json.loads(line) for line in response.iter_lines() if line]
        self.assertTrue(chunks[-1].get("done"), chunks[-1])
        calls = [
            tc for c in chunks for tc in (c.get("message", {}).get("tool_calls") or [])
        ]
        self.assertEqual(len(calls), 1, chunks)
        self.assertEqual(calls[0]["function"]["name"], "calculator_calculate")
        self.assertEqual(
            _normalize_expression(calls[0]["function"]["arguments"]["expression"]),
            "1+1",
        )


if __name__ == "__main__":
    run_server_tests(ToolCallingTests, "TOOL CALLING TESTS", modality="llm")
