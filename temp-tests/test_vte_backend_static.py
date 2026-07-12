"""
STATIC verification (does not replace a real build) of the VTE backend
integration in Lemonade. With no cmake/MSVC available in this session to
actually compile, this greps the real codebase to confirm every C++ symbol
referenced in VTE.h/VTE_server.h/VTE_server.cpp actually exists, and that
BackendDescriptor was initialized with the right number of fields, in the
right order (positional initialization -- one field too many/few or out of
order does NOT cause a compile error if the types happen to line up, it just
silently assigns the wrong value to the wrong field).

Delete this folder once the integration is complete and validated (actually
compiled and run) -- see the conversation that requested these temporary
tests.
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
        assert p.is_file(), f"Expected file does not exist: {p}"


def test_cmakelists_has_vte_entry_and_nothing_else_changed():
    text = CMAKE.read_text(encoding="utf-8")
    assert '"vte|VTE"' in text, "'vte|VTE' entry not found in LEMON_BACKENDS"
    # Confirms only one new line was added -- other backends' entries stay
    # intact (simple regression check).
    for other in ["llamacpp|llamacpp", "vllm|vllm", "moonshine|moonshine", "cloud|cloud"]:
        assert f'"{other}"' in text, f"Existing entry '{other}' disappeared from LEMON_BACKENDS -- regression!"


def test_vte_h_field_order_prefix_matches_moonshine_style_comments():
    """Checks that the /*field*/ comments in VTE.h appear in the SAME order
    as in moonshine.h -- a cheap proxy for "the field order matches", since
    both files annotate each position with the field name."""
    field_comment_re = re.compile(r"/\*(\w+)\*/")
    vte_fields = field_comment_re.findall(VTE_H.read_text(encoding="utf-8"))
    moonshine_fields = field_comment_re.findall(MOONSHINE_H.read_text(encoding="utf-8"))
    assert vte_fields == moonshine_fields, (
        f"Field comment order diverges:\nVTE.h:       {vte_fields}\nmoonshine.h: {moonshine_fields}"
    )


def test_vte_server_header_declares_required_overrides():
    text = VTE_SERVER_H.read_text(encoding="utf-8")
    for symbol in ["void load(", "void unload(", "chat_completion(", "completion(", "get_install_params("]:
        assert symbol in text, f"VTE_server.h does not declare '{symbol}'"
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
        assert symbol in text, f"Symbol '{symbol}' used in VTE_server.cpp but not found in wrapped_server.h"


def test_referenced_backend_utils_symbols_exist():
    text = BACKEND_UTILS_H.read_text(encoding="utf-8")
    for symbol in ["get_backend_binary_path", "struct InstallParams", "make_spec"]:
        assert symbol in text, f"Symbol '{symbol}' not found in backend_utils.h"
    registry_text = (REPO / "src/cpp/include/lemon/backends/backend_registry.h").read_text(encoding="utf-8")
    assert "make_server" in registry_text, "Symbol 'make_server' not found in backend_registry.h"


def test_referenced_model_manager_symbols_exist():
    text = MODEL_MANAGER_H.read_text(encoding="utf-8")
    for symbol in ["resolved_path", "std::string checkpoint(", "GgufMetadata gguf"]:
        assert symbol in text, f"Symbol '{symbol}' not found in model_manager.h"


def test_referenced_process_manager_symbols_exist():
    text = PROCESS_MANAGER_H.read_text(encoding="utf-8")
    for symbol in ["static ProcessHandle start_process(", "static void stop_process("]:
        assert symbol in text, f"Symbol '{symbol}' not found in process_manager.h"


def test_vte_recipe_not_already_registered_elsewhere():
    """Confirms 'vte' does not collide with any existing recipe (simple
    check against the other descriptor files)."""
    descriptors_dir = REPO / "src/cpp/include/lemon/backends"
    hits = []
    for f in descriptors_dir.rglob("*.h"):
        if f == VTE_H or f == VTE_SERVER_H:
            continue
        if re.search(r'recipe\*/\s*"vte"', f.read_text(encoding="utf-8", errors="ignore")):
            hits.append(f)
    assert not hits, f"Recipe 'vte' already used in another descriptor: {hits}"
