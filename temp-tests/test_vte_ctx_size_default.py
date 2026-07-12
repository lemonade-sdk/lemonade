"""
Verifica a correcao do default de ctx_size especifico do recipe "vte"
(8192, nao os 32768 que o auto-tune generico do Lemonade escolhia sem um
ctx_size explicito).

Historico (nao repetir esse erro): a primeira tentativa colocou
`config_extra: {{"ctx_size", 8192}}` no BackendDescriptor (VTE.h), o que
parecia certo lendo o codigo, mas medido de verdade (build real + load real)
o valor NUNCA era aplicado -- `RuntimeConfig::recipe_options()` so traduz
chaves que o descriptor declara em `options`, e o tratamento especial de
ctx_size so le a chave de nivel RAIZ do config.json, nunca uma por-recipe.
A correcao real e via `recipe_options` no proprio server_models.json (nivel
de modelo, nao de recipe) -- confirmado end-to-end: KV Cache Pool caiu de
896.0MB (ctx=32768) para 224.0MB (ctx=8192), Activation Arena de 672.0MB
para 168.0MB, e o tok/s de decode voltou para ~100 (era 80-88 em ctx=32768).

Apagar esta pasta quando a integracao estiver completa e validada.
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
        "O default de ctx_size=8192 precisa vir de 'recipe_options' no "
        "proprio model entry -- 'config_extra' no BackendDescriptor NAO "
        "funciona para ctx_size (ver docstring deste arquivo)."
    )


def test_vte_h_does_not_use_broken_config_extra_ctx_size():
    """Confere o campo config_extra EM SI (nao o arquivo inteiro, que
    legitimamente menciona 'ctx_size", 8192' dentro do comentario explicando
    por que isso nao funciona)."""
    text = VTE_H.read_text(encoding="utf-8")
    m = re.search(r"/\*config_extra\*/\s*(.+?),\s*\n\};", text, re.S)
    assert m, "Nao encontrei o campo config_extra na inicializacao do descriptor"
    config_extra_value = m.group(1).strip()
    assert "ctx_size" not in config_extra_value, (
        f"config_extra ainda contem 'ctx_size' ({config_extra_value!r}) -- isso "
        "nao tem efeito real (confirmado testando um build real, ver docstring "
        "deste arquivo). Nao reintroduzir sem antes corrigir "
        "RuntimeConfig::recipe_options() para aceitar o nome do recipe e ler a "
        "secao por-recipe do proprio uses_ctx_size."
    )
