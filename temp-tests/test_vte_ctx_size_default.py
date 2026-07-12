"""
Verifies the fix for the "vte" recipe's ctx_size default (8192, not the
32768 Lemonade's generic auto-tune picked without an explicit ctx_size).

History (do not repeat this mistake): the first attempt put
`config_extra: {{"ctx_size", 8192}}` in the BackendDescriptor (VTE.h), which
looked correct reading the code, but measured for real (real build + real
load) the value was NEVER applied -- `RuntimeConfig::recipe_options()` only
translates keys the descriptor declares in `options`, and the special-cased
handling of ctx_size only reads the ROOT-level key of config.json, never a
per-recipe one. The real fix is via `recipe_options` on the model entry
itself in server_models.json (model level, not recipe level) -- confirmed
end to end: KV Cache Pool dropped from 896.0MB (ctx=32768) to 224.0MB
(ctx=8192), Activation Arena from 672.0MB to 168.0MB, and decode tok/s went
back to ~100 (was 80-88 at ctx=32768).

Delete this folder once the integration is complete and validated.
"""
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SERVER_MODELS = REPO / "src/cpp/resources/server_models.json"
VTE_H = REPO / "src/cpp/include/lemon/backends/VTE/VTE.h"


def test_server_models_json_has_ctx_size_override():
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    entry = data["Qwen2.5-1.5B-Instruct-VTE"]
    assert entry.get("recipe_options", {}).get("ctx_size") == 8192, (
        "The ctx_size=8192 default needs to come from 'recipe_options' on "
        "the model entry itself -- 'config_extra' on the BackendDescriptor "
        "does NOT work for ctx_size (see this file's docstring)."
    )


def test_vte_h_does_not_use_broken_config_extra_ctx_size():
    """Checks the config_extra field ITSELF (not the whole file, which
    legitimately mentions 'ctx_size", 8192' inside the comment explaining
    why that doesn't work)."""
    text = VTE_H.read_text(encoding="utf-8")
    m = re.search(r"/\*config_extra\*/\s*(.+?),\s*\n\};", text, re.S)
    assert m, "Could not find the config_extra field in the descriptor initialization"
    config_extra_value = m.group(1).strip()
    assert "ctx_size" not in config_extra_value, (
        f"config_extra still contains 'ctx_size' ({config_extra_value!r}) -- this "
        "has no real effect (confirmed by testing a real build, see this file's "
        "docstring). Do not reintroduce it without first fixing "
        "RuntimeConfig::recipe_options() to accept the recipe name and read its "
        "own uses_ctx_size per-recipe section."
    )
