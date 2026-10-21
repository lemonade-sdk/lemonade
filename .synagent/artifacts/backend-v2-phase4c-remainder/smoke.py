import json
import pathlib
import sys
import tempfile

root = pathlib.Path("/devel/tools/lemonade")
sys.path.insert(0, str(root / "test"))
from backend_catalog_http import Server, prepare, request

with tempfile.TemporaryDirectory(prefix="lemon-4c-smoke-") as temporary:
    folder = pathlib.Path(temporary)
    prepare(folder)
    server = Server(root / "build/lemond", folder, "--builtin-backends-only")
    with server:
        health = request(server.port, "/v1/health")[0]
        schema_status, schema, _ = request(server.port, "/v1/backends/schema")
        result = {
            "port": server.port,
            "health": health,
            "schema_status": schema_status,
            "recipes": len(schema["recipes"]),
            "definitions": sum(
                row["definition_id"] is not None for row in schema["recipes"]
            ),
        }
    result["exit_code"] = server.process.returncode
    assert (
        result["health"] == result["schema_status"] == 200
        and result["recipes"] == 17
        and result["definitions"] == 8
        and result["exit_code"] == 0
    )
    (root / ".synagent/artifacts/backend-v2-phase4c-remainder/smoke.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print(result)
