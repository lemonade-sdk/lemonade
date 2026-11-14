#pragma once

#include "backend_typed_hook_registry_internal.h"

namespace runtime_fixture {
inline void raw_label_contract() {
    const auto parity = Json::parse(read_bytes("c6.raw-labels.json"));
    auto doc = runtime_document();
    auto& merge = doc["merge"];
    merge["hooks"].erase("launch");
    merge["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
    merge["launch"]["argv"] = parity.at("argv");
    merge["launch"]["custom_args"] = parity.at("custom_args");
    for (const auto* role : {"main", "mmproj", "draft"}) merge["artifacts"][role] = {
        {"role", role}, {"strategy", "resolved_path"}, {"missing_error", "fixture artifact missing"}};
    const auto bound = executable(doc);
    const auto entry = ConfiguredBackendEntry::make(bound);
    DefinitionRunner runner(bound);
    Launch launch;
    for (const auto& row : parity.at("cases")) {
        auto info = model();
        info.labels = row.at("labels").get<std::vector<std::string>>();
        info.checkpoints["draft"] = row.at("draft_checkpoint");
        info.resolved_paths = {{"main", "/fixture/main.fake"}, {"mmproj", "/fixture/vision.fake"}, {"draft", "/fixture/draft.fake"}};
        RecipeOptions options;
        options.set_option("ctx_size", 4096);
        options.set_option("fork_args", row.at("custom_args"));
        runner.resolve_runtime_options(info, options);
        const auto prepared = runner.prepare_load({"fixture", info, options, "error", {}, {}}, entry->spec, launch);
        const auto& labels = prepared.session->frame.values.at("model").at("labels");
        const auto name = row.at("name").get<std::string>();
        check(labels.size() == 2 && labels.at("mtp").is_boolean() && labels.at("dflash").is_boolean() &&
            labels.at("mtp") == row.at("raw_mtp") && labels.at("dflash") == row.at("raw_dflash"), "RawLabelFacts:" + name);
        check(prepared.plan.command.args == row.at("argv").get<std::vector<std::string>>(), "RawMtpStackArgv:" + name);
        const auto effective = typed::llamacpp::runtime_options({info.labels, info.checkpoint("draft")});
        check(effective.mtp == row.at("effective_mtp") && effective.dflash == row.at("effective_dflash"), "EffectiveSpeculationUnchanged:" + name);
    }
    auto typed_doc = doc;
    typed_doc["merge"]["launch"]["argv"] = Json::array({{{"id", "raw-dflash"}, {"args", {"--raw-dflash"}},
        {"when", {{"ref", "/model/labels/dflash"}}}}});
    const auto typed_bound = executable(typed_doc);
    const auto& ir = ex::Inspection::program(typed_bound->compiled());
    check(ir.expressions.at("/launch/argv/0/when").type == "boolean", "RawDflashInferredBoolean");
    for (const auto* path : {"/model/labels", "/model/labels/other", "/model/labels/mtp/child"}) {
        auto changed = typed_doc;
        changed["merge"]["launch"]["argv"][0]["when"] = {{"ref", path}};
        check(throws([&] { executable(changed); }), std::string("RawLabelDomainRejects:") + path);
    }
    for (const auto* type : {"string", "number", "integer"}) {
        auto changed = typed_doc;
        changed["merge"]["launch"]["argv"][0]["when"] = {{"ref", "/model/labels/mtp"}, {"type", type}};
        check(throws([&] { executable(changed); }), std::string("RawLabelTypeRejects:") + type);
    }
    auto changed = typed_doc;
    changed["merge"]["launch"]["argv"][0]["when"]["default"] = "false";
    check(throws([&] { executable(changed); }), "RawLabelDefaultMustBeBoolean");
}
}
