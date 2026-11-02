"""Independently derive C9's single closed DS4 memory-timing schema addition."""

import json
from pathlib import Path
import subprocess

from container_c9_control import BASE

REPO = Path(__file__).resolve().parents[1]
SCHEMA = "schemas/backend-definition-v2.schema.json"
RECORDS = [
    {
        "op": "add",
        "path": "/$defs/ExecutionCase/oneOf/1/properties/memory_phase",
        "new": {"enum": ["after_ports"]},
    }
]


def schema_bytes(ds4):
    original = subprocess.check_output(
        ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
    )
    if not ds4:
        return original
    value = json.loads(original)
    properties = value["$defs"]["ExecutionCase"]["oneOf"][1]["properties"]
    assert "memory_phase" not in properties, "C9MemoryPhaseIsNew"
    properties["memory_phase"] = {"enum": ["after_ports"]}
    return (json.dumps(value, indent=2) + "\n").encode()
