#include "backend_definition_exports_internal.h"

namespace lemon::backends {

BuiltinExportContract llamacpp_external_contract() {
    BuiltinExportContract policy{{"llamacpp", 1},
        "sha256:e8ea19f3af8db0b892cb5d3c9d5e217c944517734fbe26027d7270742e2371da",
        "sha256:8ba9eb7509e84d8f93f5362ce0839dc2dede2691d6a99d7987cf8099bcdd606a",
        "sha256:f75b38c730afe832f8c4d566549c51c10e34a800e0b3f1dbf2a2cf507bfcdb63",
        1, "sha256:7e4a67f7dbb82b146277e5fafd994e62f33d789ef758adb872164827f7bc0976", {}};
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Root, "backend", "backend", "llamacpp.backend.v1"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:1a11a420b26c84d99075d481e529c340df7ab9df29dda16b671996f76aeaed1c", SlotRemoval::Forbidden, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:ad7808ae0f3ae836c7816b8d043a9d06582679daee2af6a1d8e0e94f3854f15b";
        slot.ordered_hooks = {"llamacpp.backend.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Root, "environment", "environment", "llamacpp.environment.v1"},
            3, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:b476b1484d27e5f6d3f2277a68e034b4b5dd35bbc1f318d98c40bdf0e9e24a1f", SlotRemoval::Independent, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:48ebf2bfcba632794f20fdb5c0b71c65f1c19eac8b92ab9877dba82810a876c9";
        slot.ordered_hooks = {"llamacpp.environment.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Root, "install_context", "install_context", "llamacpp.install_context.v1"},
            2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:b2d8a190b01087107d451f30a34a34079bc95fd6b5690f141b3a28528d2d2122", SlotRemoval::Independent, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:e575b9d43bd1b631293a8a82343e92de870cdba72b95ce35d3dbe8c972bcf9ab";
        slot.ordered_hooks = {"llamacpp.install_context.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Root, "launch", "launch", "llamacpp.prepare.v1"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:0c518bab0502c145d48e7f724e283d306e2a7aa9ee922c0fcc674c9e989563a9", SlotRemoval::Forbidden, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:34554e16d632b4823d1f9bd93a6cd5b275ae8a342d236dbc60b862f5dd10c1f6";
        slot.ordered_hooks = {"llamacpp.prepare.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Root, "runtime_options", "runtime_options", "llamacpp.runtime_options.v1"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:88271b57a10806885f0885c26b766f8e3df255b097072ca072d4532b67d6b484", SlotRemoval::Independent, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:0b25f5b68b2e1fcb6c12b544cfdbad2febd614bef14f1da43458b9144cb18e8a";
        slot.ordered_hooks = {"llamacpp.runtime_options.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Root, "system", "system", "llamacpp.system.v1"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:f89391ecb059dc3996ce27e9144a4f6e30f1c52d06fb13972402cfe557897355", SlotRemoval::Independent, {}};
        slot.effects = {HookEffect::ProcessProbe};
        slot.owner_semantics_digest = "sha256:d40263657affbe6fec4c90bedc4f0533ac57f9bbb9eab39aea0d360ce8e746da";
        slot.ordered_hooks = {"llamacpp.system.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "main", "artifacts", "llamacpp.artifacts.v1"},
            2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:9ab071a1c0db6d52e1c0a65e78b554c022837ca4fd9f51d115f1a8910cdddb27", SlotRemoval::Forbidden, {}};
        slot.effects = {HookEffect::ManagedInstallRead};
        slot.owner_semantics_digest = "sha256:e7ae4dde49a12f08e1f40474006261c4dff420e71b782ca7b12b3bd9688535f9";
        slot.ordered_hooks = {"llamacpp.artifacts.v1"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.max_tokens_alias"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:a61d5feaba23807bb847e70cc36649b11fbbbfe0ade1e26c21f35f797a810765", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
        slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.tool_schema_limits"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:b94bcfa0eb4b19fdce4e709c17969f2006363eb0a77b5bcd4641460794eee688", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
        slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "response", "llamacpp.client_model"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:3c2617162eda5e2c9f73d6af26265e711d639fdd07ac6ca191d34898cc5a5d37", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
        slot.ordered_hooks = {"llamacpp.client_model"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "request", "llamacpp.max_tokens_alias"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:246d95ceae482b18cbd90e6789209abf5f0b8a2d7988460147fe37739352a674", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
        slot.ordered_hooks = {"llamacpp.max_tokens_alias"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "response", "llamacpp.client_model"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:72a927825d967e8c40f96ac9b669bb7fa0528c8d4199cdd7b628c77fa2784dd1", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
        slot.ordered_hooks = {"llamacpp.client_model"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "request", "llamacpp.tool_schema_limits"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:c6f9a5da6ec09edb785d0f545f974990485bbb2fea6edf2fbaf497f54db81580", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
        slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "response", "llamacpp.client_model"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:af0d1e05ac2bd421844981557c419027b63f37d91833c4a527ad00cb3340f587", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
        slot.ordered_hooks = {"llamacpp.client_model"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.max_tokens_alias"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:cd38950f889d913cb5668af57ca9932c189530455c1be21c4ac167a42f321cee", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
        slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.tool_schema_limits"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:a47627f1f65778abe084a2319bcce0cad4df75a1b1a11783a415d4c42b61500d", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
        slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
        policy.slots.push_back(std::move(slot));
    }
    {
        ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.streaming", "request", "llamacpp.tool_schema_limits"},
            1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3",
            "sha256:1d2aeff62aed9e15e3354960a156d4b4b5703e95dab65912e3bbbc8e5a62eff9", SlotRemoval::WithOwner, {}};
        slot.effects = {HookEffect::Pure};
        slot.owner_semantics_digest = "sha256:a7405961c2d5efa6d4f9a6e21521f04f8dcac0b7db902d46f9f3a221846b7a32";
        slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
        policy.slots.push_back(std::move(slot));
    }
    policy.owners = {
        {"chat.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"chat.buffered","model_modes":["*"],"operation":"chat_completion","transport":{"kind":"http_json","method":"POST","path":"/v1/chat/completions","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming", "slot-action.buffered"}},
        {"completion.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"completion.buffered","model_modes":["*"],"operation":"completion","transport":{"kind":"http_json","method":"POST","path":"/v1/completions","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {"llamacpp.max_tokens_alias"}, {"llamacpp.client_model"}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.streaming", "slot-action.buffered"}},
        {"responses.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"responses.buffered","model_modes":["*"],"operation":"responses","transport":{"kind":"http_json","method":"POST","path":"/v1/responses","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {"llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.streaming", "slot-action.buffered"}},
        {"chat.streaming", nlohmann::json::parse(R"owner({"delivery":"streaming","id":"chat.streaming","model_modes":["*"],"operation":"chat_completion","transport":{"kind":"http_sse","method":"POST","path":"/v1/chat/completions","port":"http","stream_codec":"openai_sse.v1","timeout":"global"}})owner"),
            {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "slot-action.buffered"}},
        {"completion.streaming", nlohmann::json::parse(R"owner({"delivery":"streaming","id":"completion.streaming","model_modes":["*"],"operation":"completion","transport":{"kind":"http_sse","method":"POST","path":"/v1/completions","port":"http","stream_codec":"openai_sse.v1","timeout":"global"}})owner"),
            {}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.buffered", "slot-action.buffered"}},
        {"responses.streaming", nlohmann::json::parse(R"owner({"delivery":"streaming","id":"responses.streaming","model_modes":["*"],"operation":"responses","transport":{"kind":"http_sse","method":"POST","path":"/v1/responses","port":"http","stream_codec":"openai_sse.v1","timeout":"global"}})owner"),
            {"llamacpp.tool_schema_limits"}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.buffered", "slot-action.buffered"}},
        {"embedding.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"embedding.buffered","model_modes":["*"],"operation":"embeddings","transport":{"kind":"http_json","method":"POST","path":"/v1/embeddings","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
        {"reranking.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"reranking.buffered","model_modes":["*"],"operation":"reranking","transport":{"kind":"http_json","method":"POST","path":"/v1/rerank","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
        {"slots.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"slots.buffered","model_modes":["*"],"operation":"slots","transport":{"kind":"http_json","method":"GET","path":"/slots","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
        {"slot-action.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"slot-action.buffered","model_modes":["*"],"operation":"slot_action","transport":{"kind":"http_json","method":"POST","path":"/slots/{slot_id}?action={action}","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "completion.buffered", "responses.buffered", "chat.streaming", "completion.streaming", "responses.streaming", "embedding.buffered", "reranking.buffered", "slots.buffered", "tokenize.buffered"}},
        {"tokenize.buffered", nlohmann::json::parse(R"owner({"delivery":"buffered","id":"tokenize.buffered","model_modes":["*"],"operation":"tokenize","transport":{"kind":"http_json","method":"POST","path":"/tokenize","port":"http","response_codec":"json.v1","timeout":"global"}})owner"),
            {}, {}, {}, true, {},
            {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
    };
    policy.owner_graph_digest = "sha256:6bc3f2b40d73e44ceb384564fbc403b1f58cf22c91de863f0746c5f22713e0f8";
    const auto pins = nlohmann::json::parse(R"pins({"owner_graph":"sha256:29ae8a1e9b961967b126009b7e2d3d66335b7cf6f1a8574e4c2184e72ce68d1b","policy":"sha256:fddeac59a68ca319b4fd5a9307800ab60691d61b30739e37405966b7b71ea5f6","registry":"sha256:d644387f2d2838a76ace64304318e673244e8cd117e496ae4394dc0792e2b9fa","slots":["sha256:57971cc997edbeace461bdd700caac82b127b1c2a58d0862f50394224691c5b0","sha256:e61399da847ff647f59ef76be980f4e6ff2a443cbaa6cf360db924dec4a26836","sha256:7fb0c97ce230af539796b73154b7631bf57df8d9346dcc40253b4f5e936996c4","sha256:b59051c54c35025431454b2d3b845d60dd7e651252892ae69f5e63729043f22c","sha256:89c0b78903d89a06fffe580f24be212f155da8fa3a587122b720ac27c586ff18","sha256:bbdb16f254d6acd4b5eb3042f94fb758caacc452187e4fa6e5cf598556df25b5","sha256:1073493abec936f4e0b2df24807380d2dcfbe380151625e8924e7cfa81433400","sha256:5346fff833bf07db27e532633182b22c4a11a3f4f02442e043506477f28c9c6a","sha256:da202c25060668cb86a5f592f4cfe936beb5a9c3e2b93bad7f407838d936f609","sha256:b760fa3b4a27f16ccb8c98faf344002f8a2e5e3617db42a2312e26a5e7a35094","sha256:cecbcfa10c10eb1596c89d345f921ef901cd98ad50c7aec0ca73c1a5bbbbd530","sha256:9e5a24213fd3c391fcb0e3a5b77435220e9a0b977ebc86335a0818abfe789d04","sha256:db70516f4f5d38896f2c1ee45997b5284177b20278759b2d4fb1caa8a8715539","sha256:6664531eac117cfddf79e1015d8a379bd25d71f8f838b9943cf62fa2882e478f","sha256:ee3466d39d602927e8b006b40c9675d208ed5d4d48417d689ef43e18dcb78d4d","sha256:84be9f9d543018e8b38dc35431e303c38bdcfbf6cd2dc123b828ca3f752c66cb","sha256:0b8eccb9ae6aaaf12fce2fe0c0c966c918c9fc1a3bf42a048c8dec8f31dde23a"]})pins");
    policy.mutable_fragments = {
        {MutableFragmentKind::SupportRowsV1, {"/metadata/support", {}, ""}},
        {MutableFragmentKind::CustomDefaultsV1, {"/launch/custom_args/defaults", {}, ""}},
        {MutableFragmentKind::EnvironmentEntriesV1, {"/launch/environment/set", {}, ""}},
        {MutableFragmentKind::LocalExecutableV1, {"/install", {}, ""}}
    };
    for (auto& rule : policy.mutable_fragments) {
        rule.maximum_records = 64;
        rule.maximum_length = 1024;
        if (rule.kind != MutableFragmentKind::LocalExecutableV1) rule.collection = {true, true, true};
        if (rule.kind == MutableFragmentKind::CustomDefaultsV1) {
            rule.domains = {{"stage", {"runtime_options", "launch"}}};
            rule.references = {{"/runtime/dflash", MutableValueType::Boolean, ReferencePhase::RuntimeOptions},
                {"/runtime/mtp", MutableValueType::Boolean, ReferencePhase::RuntimeOptions}};
        }
        if (rule.kind == MutableFragmentKind::EnvironmentEntriesV1)
            rule.domains = {{"name", {"GGML_DISABLE_VULKAN"}}};
        if (rule.kind == MutableFragmentKind::SupportRowsV1) rule.domains = {
            {"backend", {"system", "metal", "cuda", "vulkan", "rocm", "cpu"}},
            {"device", {"cpu", "metal", "nvidia_gpu", "amd_gpu"}},
            {"arch", {"x86_64", "arm64", "sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120", "sm_121",
                "gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X", "gfx908", "gfx90a", "gfx942", "gfx950"}},
            {"channel", {"stable", "nightly"}}};
    }
    std::vector<MutableDataRule> scalars;
    const auto string = [&](DefinitionTarget target, bool optional = false, nlohmann::json values = nlohmann::json::array()) {
        MutableDataRule rule{std::move(target), MutableValueType::String};
        rule.maximum_length = 256;
        rule.allow_absent = optional;
        rule.values = std::move(values);
        scalars.push_back(std::move(rule));
    };
    for (const auto* field : {"display_name", "server_name", "log_category", "web_display_name", "config_section"})
        string({std::string("/metadata/") + field, {}, ""});
    for (const auto* os : {"linux", "windows", "macos"}) string({std::string("/metadata/binary/") + os, {}, ""});
    string({"/metadata/default_device", {}, ""}, false, {"cpu", "gpu"});
    string({"/launch/environment/preset", {}, ""}, false, {"hook", "inherit"});
    for (const auto* id : {"backend", "device", "args"}) for (const auto* field : {"name", "help", "group", "cli_flag"})
        string({"/metadata/options", RecordKey{RecordKey::Field::Id, id}, std::string("/") + field}, std::string(field) == "cli_flag");
    policy.slots.at(6).required_slots = {{SlotOwnerKind::Root, "launch", "launch", "llamacpp.prepare.v1"}};
    for (std::size_t i = 0; i < policy.slots.size(); ++i) {
        policy.slots[i].mutable_inputs = scalars;
        policy.slots[i].mutable_fragments = policy.mutable_fragments;
        policy.slots[i].mutation_policy_digest = pins.at("slots").at(i);
    }
    policy.policy_digest = pins.at("policy");
    policy.owner_graph_digest = pins.at("owner_graph");
    return policy;
}

}
