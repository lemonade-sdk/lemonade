"""Mirror the production llama.cpp export contract into its qualified test literal and pin file."""

import argparse
import json
from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[1]
PRODUCTION = REPO / "src/cpp/server/backends/configured/backend_llamacpp_export.cpp"
LITERAL = REPO / "test/cpp/llamacpp_external_contract.h"
PINS = REPO / "test/fixtures/backend-v2/external/llamacpp.pins.json"
HEAD = (
    '#include "backend_definition_exports_internal.h"\n\n'
    "namespace lemon::backends {\n\n"
    "BuiltinExportContract llamacpp_external_contract() {\n"
)
PIN_LINE = re.compile(
    r'    const auto pins = nlohmann::json::parse\(R"pins\((.*)\)pins"\);\n'
)


def render(production):
    assert production.startswith(HEAD), "ExternalContractProductionHead"
    assert production.endswith(
        "    return policy;\n}\n\n}\n"
    ), "ExternalContractProductionTail"
    match = PIN_LINE.search(production)
    assert (
        match and len(PIN_LINE.findall(production)) == 1
    ), "ExternalContractSinglePinLiteral"
    pins = json.loads(match.group(1))
    body = production[len(HEAD) : -len("\n}\n")]
    body = body.replace(
        match.group(0),
        '    const auto pins = nlohmann::json::parse(export_fixture::read_bytes("external/llamacpp.pins.json"));\n',
    )
    literal = (
        "#pragma once\n\n"
        '#include "backend_definition_export_fixture.h"\n\n'
        "namespace lemon::backends::external_fixture {\n\n"
        "inline BuiltinExportContract contract() {\n" + body + "\n"
        "inline std::shared_ptr<const BuiltinExportRegistry> registry() {\n"
        "    const auto input = make_v2_source_snapshot(embedded_v2_sources());\n"
        '    const auto pins = nlohmann::json::parse(export_fixture::read_bytes("external/llamacpp.pins.json"));\n'
        "    return BuiltinCatalogLoader::test_exports({contract()}, input.snapshot, installed_hook_abis(), "
        'pins.at("registry").get<std::string>());\n'
        "}\n\n}\n"
    )
    return literal, pins


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["check", "write"])
    args = parser.parse_args()
    literal, pins = render(PRODUCTION.read_text())
    if args.mode == "write":
        LITERAL.write_text(literal)
        PINS.write_text(json.dumps(pins, indent=2) + "\n")
    assert LITERAL.read_text() == literal, "ExternalContractLiteralMirrorsProduction"
    assert json.loads(PINS.read_text()) == pins, "ExternalContractPinsMirrorProduction"
    print("external contract literal and pins mirror production")
