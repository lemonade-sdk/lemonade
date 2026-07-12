"""
Dois bugs pre-existentes do Lemonade (nao relacionados a integracao VTE em
si) foram encontrados e corrigidos ENQUANTO se validava a integracao de
verdade, compilando e rodando lemond.exe contra hardware real (RX 7600):

1. `/MANIFESTINPUT:` sem aspas no CMakeLists.txt quebra o link (LNK1181)
   sempre que o repositorio esta em um caminho com espaco (ex.: esta
   maquina, "Aetheris Flow").
2. `identify_rocm_arch_from_name()` nunca reconhecia "7600" como RDNA3 --
   a propria RX 7600 (a placa em que o VTE inteiro foi construido e medido)
   nunca aparecia como suportada em NENHUM backend ROCm (nem llamacpp, nem
   vte), so por causa dessa lacuna no reconhecimento por nome.

Ambos confirmados corrigidos numa build real (cmake --build --preset
windows) e num lemond.exe real rodando (RX 7600 passou a aparecer como
"gfx110X" em /api/v1/system-info, e vte/llamacpp passaram de "unsupported"
para "installable").

Apagar esta pasta quando a integracao estiver completa e validada.
"""
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKE = REPO / "CMakeLists.txt"
SYSTEM_INFO_CPP = REPO / "src/cpp/server/system_info.cpp"


def test_manifest_input_path_is_quoted():
    text = CMAKE.read_text(encoding="utf-8")
    assert '/MANIFESTINPUT:\\"${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest\\"' in text, (
        "A flag /MANIFESTINPUT: precisa envolver o path em aspas escapadas -- "
        "sem isso, o link de lemond.exe quebra (LNK1181) em qualquer checkout "
        "cujo caminho contenha um espaco (confirmado numa build real nesta sessao)."
    )


def test_rx7600_recognized_as_gfx110x():
    text = SYSTEM_INFO_CPP.read_text(encoding="utf-8")
    # A mesma condicao que já reconhece 7700/7800/7900/v710 como gfx110X
    # precisa também checar "7600" -- sem isso a RX 7600 nunca aparece como
    # RDNA3 suportada em NENHUM backend ROCm (confirmado rodando um lemond.exe
    # real: GPU family vinha "" antes da correção, "gfx110X" depois).
    idx = text.find('return "gfx110X"')
    assert idx != -1, "Bloco de deteccao gfx110X nao encontrado em system_info.cpp"
    preceding = text[max(0, idx - 400):idx]
    assert '"7600"' in preceding, (
        "'7600' precisa aparecer na cadeia de checagens que precede 'return \"gfx110X\"' "
        "-- sem isso a RX 7600 nunca e reconhecida como RDNA3 por nome."
    )
