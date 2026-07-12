"""
Verifica as entradas de configuracao (JSON/YAML/Python) adicionadas para o
backend VTE: server_models.json, backend_versions.json,
test/utils/capabilities.py e a linha nova na matriz de CI. Nao substitui
rodar a suite de teste real (test/server_llm.py --wrapped-server vte
--backend rocm) contra um lemond compilado -- isso ainda depende de cmake.

Apagar esta pasta quando a integracao estiver completa e validada.
"""
import json
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parent.parent
SERVER_MODELS = REPO / "src/cpp/resources/server_models.json"
BACKEND_VERSIONS = REPO / "src/cpp/resources/backend_versions.json"
CAPABILITIES_PY = REPO / "test/utils/capabilities.py"
CI_WORKFLOW = REPO / ".github/workflows/cpp_server_build_test_release.yml"


def test_server_models_json_valid_and_has_vte_entry():
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    assert "Qwen2.5-1.5B-Instruct-VTE" in data
    entry = data["Qwen2.5-1.5B-Instruct-VTE"]
    assert entry["recipe"] == "vte"
    assert entry["checkpoint"] == "Qwen/Qwen2.5-1.5B-Instruct-GGUF:qwen2.5-1.5b-instruct-q4_k_m.gguf"
    # suggested=True is what makes a model show up in Model Manager's browse/download
    # list at all (ModelManager.tsx's suggestedModels filters on `info.suggested`) --
    # NOT an endorsement that VTE is production-ready. The backend's own
    # `experimental: true` flag (VTE.h) is what marks it opt-in; "suggested" is a
    # visibility switch, a distinction found the hard way: Granite/Qwen3.5-VTE were
    # unreachable from the UI with suggested=False, even though they were fully
    # registered and downloadable via the API.
    assert entry["suggested"] is True


def test_server_models_json_has_granite_and_qwen35_vte_entries():
    """Granite (Q8_0) e Qwen3.5 (Q6_K) sao as outras duas arquiteturas que o
    VTE sabe rodar (ver vte/compiler/sanitizer.py::SUPPORTED_ARCHITECTURES) --
    ambos os checkpoints foram baixados e carregados de verdade (VTEModel.
    from_pretrained + generate() real, nao so lidos do Hub) antes de entrar
    aqui, confirmando que a quantizacao de cada um funciona no VTE."""
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))

    granite = data["Granite-4.1-3B-VTE"]
    assert granite["recipe"] == "vte"
    assert granite["checkpoint"] == "unsloth/granite-4.1-3b-GGUF:granite-4.1-3b-Q8_0.gguf"
    assert granite["suggested"] is True  # see comment on the Qwen2.5-VTE assertion above
    assert granite["recipe_options"]["ctx_size"] == 8192  # mesmo fix de VRAM/tok-s do Qwen2.5-1.5B-Instruct-VTE

    qwen35 = data["Qwen3.5-2B-VTE"]
    assert qwen35["recipe"] == "vte"
    assert qwen35["checkpoint"] == "unsloth/Qwen3.5-2B-GGUF:Qwen3.5-2B-Q6_K.gguf"
    assert qwen35["suggested"] is True
    assert qwen35["recipe_options"]["ctx_size"] == 8192


def test_server_models_json_no_existing_entry_modified():
    """Confirma que a unica mudanca no arquivo foi a adicao -- nao apagou
    nem alterou nenhuma entrada existente (checagem simples de regressao:
    alguns nomes conhecidos de outras entradas continuam presentes)."""
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    for known_model in ["Qwen3-0.6B-GGUF", "Tiny-Test-Model-GGUF", "Qwen3-1.7B-GGUF"]:
        assert known_model in data, f"Entrada existente '{known_model}' sumiu -- regressao!"


def test_backend_versions_json_valid_and_has_vte_entry():
    data = json.loads(BACKEND_VERSIONS.read_text(encoding="utf-8"))
    # 0.2.0: release real no GitHub (kyuubyN/VTE, tag "0.2.0", sem prefixo "v"
    # -- ver o release 0.1.0 anterior, mesma convencao) com /v1/models, lock
    # de geracao e downloader. Validado ao vivo antes de publicar.
    assert data.get("vte") == {"rocm": "0.2.0"}
    # Regressao: vllm continua intacto.
    assert data.get("vllm") == {"rocm": "vllm0.20.1-rocm7.12.0"}


def test_capabilities_py_has_vte_entry_matching_moonshine_shape():
    sys.path.insert(0, str(CAPABILITIES_PY.parent))
    import capabilities  # noqa: E402

    vte_entry = capabilities.CAPABILITIES["llm"]["vte"]
    assert vte_entry["backends"] == ["rocm"]
    assert vte_entry["test_models"] == {"llm": "Qwen2.5-1.5B-Instruct-VTE"}

    required_keys = set(capabilities.CAPABILITIES["llm"]["ryzenai"]["supports"].keys())
    assert set(vte_entry["supports"].keys()) == required_keys, (
        "O dict 'supports' de vte precisa ter EXATAMENTE as mesmas chaves que "
        "outro backend LLM ja existente (ryzenai) -- uma chave faltando quebra "
        "skip_if_unsupported silenciosamente (trata como 'nao suportado')."
    )
    # O que realmente implementamos e testamos de verdade na Fase A.
    assert vte_entry["supports"]["chat_completions"] is True
    assert vte_entry["supports"]["chat_completions_streaming"] is True
    # O que NAO implementamos ainda -- nao afirmar suporte que nao existe.
    assert vte_entry["supports"]["completions_streaming"] is False
    assert vte_entry["supports"]["embeddings"] is False
    assert vte_entry["supports"]["reranking"] is False
    assert vte_entry["supports"]["tool_calls"] is False


def test_ci_workflow_yaml_valid_and_has_vte_row():
    workflow = yaml.safe_load(CI_WORKFLOW.read_text(encoding="utf-8"))
    matrix_include = workflow["jobs"]["test-exe-inference"]["strategy"]["matrix"]["include"]
    vte_rows = [row for row in matrix_include if row.get("name") == "vte"]
    assert len(vte_rows) == 1, "Deveria haver exatamente uma linha 'vte' na matriz de CI"
    row = vte_rows[0]
    assert row["extra_args"] == "--wrapped-server vte"
    assert row["backends"] == "rocm"
    assert "Windows" in row["runner"]
    assert "rocm" in row["runner"]


def test_ci_workflow_existing_rows_not_modified():
    workflow = yaml.safe_load(CI_WORKFLOW.read_text(encoding="utf-8"))
    matrix_include = workflow["jobs"]["test-exe-inference"]["strategy"]["matrix"]["include"]
    names = [row.get("name") for row in matrix_include]
    for known in ["llamacpp", "ryzenai", "flm", "moonshine"]:
        assert known in names, f"Linha de CI existente '{known}' sumiu -- regressao!"
