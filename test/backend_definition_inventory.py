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
DORMANT_CONTAINERS = {
    "gufo": "gufo",
    "rocmfpx": "rocmfpx",
    "halobox": "llamacpp",
    "nathanw": "llamacpp",
    "halogen": "halogen",
    "ds4_rocm": "ds4",
}


def production_inventory(root=REPO):
    sources = root / "src/cpp/resources/backends"
    names = {path.stem for path in sources.glob("*.json")}
    dormant_names = names & DORMANT_CONTAINERS.keys()
    dormant = "gufo" in names
    mixed = names == V1_NAMES | {"llamacpp"} | dormant_names
    assert names == V1_NAMES or mixed, "ExactMixedDefinitionSources"
    promoted_hrx = (
        json.loads((sources / "hrx.json").read_bytes())["schema_version"] == 2
    )
    for name in V1_NAMES - ({"hrx"} if promoted_hrx else set()):
        assert (
            json.loads((sources / f"{name}.json").read_bytes())["schema_version"] == 1
        ), "ExplicitEightV1Definitions"
    if promoted_hrx:
        assert mixed, "HrxChildRequiresQualifiedLlamaParent"
        hrx = json.loads((sources / "hrx.json").read_bytes())
        assert hrx["id"] == "hrx" and hrx["extends"] == {
            "builtin": "llamacpp",
            "contract": 1,
        }, "ExactHrxBuiltinChild"
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
    if dormant:
        value = json.loads((sources / "gufo.json").read_bytes())
        assert (
            value["schema_version"] == 2 and value["id"] == value["recipe"] == "gufo"
        ), "ExactDormantGufoSource"
        assert value["merge"]["kind"] == "container", "DormantGufoIsContainerData"
    for name in dormant_names:
        value = json.loads((sources / f"{name}.json").read_bytes())
        assert (
            value["schema_version"] == 2
            and value["id"] == name
            and value["recipe"] == DORMANT_CONTAINERS[name]
            and value["merge"]["kind"] == "container"
        ), "ExactDormantContainerSource"
    return names
