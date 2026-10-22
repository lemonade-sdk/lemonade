"""Exact production source partition before and after the llama.cpp promotion."""

import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
V1_NAMES = frozenset(
    {
        "ds4",
        "hrx",
        "kokoro",
        "moonshine",
        "onnxruntime",
        "ryzenai",
        "thinksound",
        "trellis",
    }
)


def production_inventory(root=REPO):
    sources = root / "src/cpp/resources/backends"
    names = {path.stem for path in sources.glob("*.json")}
    mixed = names == V1_NAMES | {"llamacpp"}
    assert names == V1_NAMES or mixed, "ExactMixedDefinitionSources"
    for name in V1_NAMES:
        assert (
            json.loads((sources / f"{name}.json").read_bytes())["schema_version"] == 1
        ), "ExplicitEightV1Definitions"
    if mixed:
        data = (sources / "llamacpp.json").read_bytes()
        assert (
            data
            == (root / "test/fixtures/backend-v2/llamacpp.candidate.json").read_bytes()
        ), "SoleAdditionalSourceIsQualifiedLlamaCandidate"
        value = json.loads(data)
        assert (
            value["schema_version"] == 2 and value["id"] == "llamacpp"
        ), "SoleAdditionalSourceIsV2Llama"
    return names
