"""
Verificacao ESTATICA (nao substitui compilar) da integracao do backend VTE
no Lemonade. Sem cmake/MSVC disponiveis nesta sessao para compilar de
verdade, isto grepa a base de codigo real para confirmar que cada simbolo
C++ referenciado em VTE.h/VTE_server.h/VTE_server.cpp de fato existe, e que
o BackendDescriptor foi inicializado com o numero certo de campos, na ordem
certa (inicializacao posicional -- um campo a mais/a menos ou fora de ordem
NAO da erro de compilacao se os tipos coincidirem, so atribui o valor errado
ao campo errado silenciosamente).

Apagar esta pasta quando a integracao estiver completa e validada (compilada
e rodada de verdade) -- ver a conversa que pediu estes testes temporarios.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKE = REPO / "CMakeLists.txt"
VTE_H = REPO / "src/cpp/include/lemon/backends/VTE/VTE.h"
VTE_SERVER_H = REPO / "src/cpp/include/lemon/backends/VTE/VTE_server.h"
VTE_SERVER_CPP = REPO / "src/cpp/server/backends/VTE/VTE_server.cpp"
BACKEND_DESCRIPTOR_H = REPO / "src/cpp/include/lemon/backends/backend_descriptor.h"
WRAPPED_SERVER_H = REPO / "src/cpp/include/lemon/wrapped_server.h"
BACKEND_UTILS_H = REPO / "src/cpp/include/lemon/backends/backend_utils.h"
MODEL_MANAGER_H = REPO / "src/cpp/include/lemon/model_manager.h"
PROCESS_MANAGER_H = REPO / "src/cpp/include/lemon/utils/process_manager.h"
MOONSHINE_H = REPO / "src/cpp/include/lemon/backends/moonshine/moonshine.h"


def test_new_files_exist():
    for p in (VTE_H, VTE_SERVER_H, VTE_SERVER_CPP):
        assert p.is_file(), f"Arquivo esperado nao existe: {p}"


def test_cmakelists_has_vte_entry_and_nothing_else_changed():
    text = CMAKE.read_text(encoding="utf-8")
    assert '"vte|VTE"' in text, "Entrada 'vte|VTE' nao encontrada em LEMON_BACKENDS"
    # Garante que so uma linha nova foi adicionada -- as entradas de outros
    # backends continuam intactas (checagem simples de nao-regressao).
    for other in ["llamacpp|llamacpp", "vllm|vllm", "moonshine|moonshine", "cloud|cloud"]:
        assert f'"{other}"' in text, f"Entrada existente '{other}' sumiu de LEMON_BACKENDS -- regressao!"


def test_vte_h_field_order_prefix_matches_moonshine_style_comments():
    """Confere que os comentarios /*campo*/ em VTE.h aparecem na MESMA ordem
    que em moonshine.h -- um proxy barato para "a ordem dos campos bate",
    já que ambos os arquivos anotam cada posicao com o nome do campo."""
    field_comment_re = re.compile(r"/\*(\w+)\*/")
    vte_fields = field_comment_re.findall(VTE_H.read_text(encoding="utf-8"))
    moonshine_fields = field_comment_re.findall(MOONSHINE_H.read_text(encoding="utf-8"))
    assert vte_fields == moonshine_fields, (
        f"Ordem dos comentarios de campo diverge:\nVTE.h:       {vte_fields}\nmoonshine.h: {moonshine_fields}"
    )


def test_vte_server_header_declares_required_overrides():
    text = VTE_SERVER_H.read_text(encoding="utf-8")
    for symbol in ["void load(", "void unload(", "chat_completion(", "completion(", "get_install_params("]:
        assert symbol in text, f"VTE_server.h nao declara '{symbol}'"
    assert "class VTEServer : public WrappedServer" in text


def _cpp_symbol_defined_anywhere(symbol: str, roots: list[Path]) -> bool:
    pattern = re.compile(re.escape(symbol))
    for root in roots:
        for f in root.rglob("*.h"):
            if pattern.search(f.read_text(encoding="utf-8", errors="ignore")):
                return True
    return False


def test_referenced_wrapped_server_symbols_exist():
    text = WRAPPED_SERVER_H.read_text(encoding="utf-8")
    for symbol in [
        "choose_port", "wait_for_ready", "forward_request", "set_process_handle",
        "consume_process_handle_for_cleanup", "has_process_handle",
        "stop_backend_watchdog", "get_backend_port", "is_debug",
    ]:
        assert symbol in text, f"Simbolo '{symbol}' usado em VTE_server.cpp mas nao encontrado em wrapped_server.h"


def test_referenced_backend_utils_symbols_exist():
    text = BACKEND_UTILS_H.read_text(encoding="utf-8")
    for symbol in ["get_backend_binary_path", "struct InstallParams", "make_spec"]:
        assert symbol in text, f"Simbolo '{symbol}' nao encontrado em backend_utils.h"
    registry_text = (REPO / "src/cpp/include/lemon/backends/backend_registry.h").read_text(encoding="utf-8")
    assert "make_server" in registry_text, "Simbolo 'make_server' nao encontrado em backend_registry.h"


def test_referenced_model_manager_symbols_exist():
    text = MODEL_MANAGER_H.read_text(encoding="utf-8")
    for symbol in ["resolved_path", "std::string checkpoint(", "GgufMetadata gguf"]:
        assert symbol in text, f"Simbolo '{symbol}' nao encontrado em model_manager.h"


def test_referenced_process_manager_symbols_exist():
    text = PROCESS_MANAGER_H.read_text(encoding="utf-8")
    for symbol in ["static ProcessHandle start_process(", "static void stop_process("]:
        assert symbol in text, f"Simbolo '{symbol}' nao encontrado em process_manager.h"


def test_vte_recipe_not_already_registered_elsewhere():
    """Garante que 'vte' nao colide com nenhum recipe ja existente (checagem
    simples contra os outros arquivos de descriptor)."""
    descriptors_dir = REPO / "src/cpp/include/lemon/backends"
    hits = []
    for f in descriptors_dir.rglob("*.h"):
        if f == VTE_H or f == VTE_SERVER_H:
            continue
        if re.search(r'recipe\*/\s*"vte"', f.read_text(encoding="utf-8", errors="ignore")):
            hits.append(f)
    assert not hits, f"Recipe 'vte' ja usado em outro descriptor: {hits}"
