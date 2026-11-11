#include "backend_definition_exports_internal.h"

namespace lemon::backends {

void append_active_container_policies(BuiltinExecutionPolicies& policies) {
    {
        BuiltinExportContract leaf{{"gufo", 1}, "sha256:d7a7ba95e170ebca097540555ed8e3880027cb034b2767623ad6b27f26e4d1df",
            "sha256:894b8148f9accb4fb203d9403a336ca48082e764803984240b8891360dc2a7cb", "sha256:cf09b42f009378ddbcd36034ba281df53a412b1bc505aa220adc68c19a15fce0",
            1, "sha256:57ff6278a348acc2b52a7ecb7ee30feb6e323af0de6e6a986437776f0680c984", {}};
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "draft", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:6bc33df18af3604191521a53e01bb16e4af7c90c1ca7d48c557fa3ae08698899", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:cbde9640caedc7ef0e59e7bc8f9a6df114f8277d5cb1c6b03d6e2bd123c31ce6";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "main", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:c8c7635e4811e80468d2236ac606ba1deeba55c14473cc2de5073838e284520b", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e744edae91a1d294130f2f30f091bb69ad06da85c677829ba56fcaeee70b8c65";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "mmproj", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:d290aac9420609a189e332f85b01b42cd0143ef09a8ede66ceae3529b58e27c6", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:3d9e4db2d99edb11f120ae5952841bbe683ef91a913b7fca1d6a4edd753b7bb9";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        leaf.owners = {
            {"chat.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"chat.buffered\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming"}},
            {"chat.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"chat.streaming\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered"}},
            {"completion.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"completion.buffered\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.streaming"}},
            {"completion.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"completion.streaming\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.buffered"}},
            {"responses.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"responses.buffered\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.streaming"}},
            {"responses.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"responses.streaming\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.buffered"}},
        };
        leaf.owner_graph_digest = "sha256:d92b0ebdd99222a23be96ef492a3f79159a2ef4b4a5e44fd8d32cf5687a2adbf";
        policies.leaves.push_back(std::move(leaf));
        policies.execution_owners.push_back(reviewed_gufo_execution_owner());
    }
    {
        BuiltinExportContract leaf{{"rocmfpx", 1}, "sha256:54adfb7620bad2ae7dec742a03516bc694ecc749e243d0d02726ce1a27b3367c",
            "sha256:d0cb45ac5934f0870536c854f19306684ddf4b3b6b1104da108e75c062d1cb49", "sha256:9b33d01726db9e4401629bfeb094100e86a09e63fa2e7b0184513243556ef415",
            1, "sha256:7faca844abdc42a3360e0580880f4006c5b15ba437b157aa6e0c29447eef2cbd", {}};
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "draft", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:d50ac562cef8dacaa50ca0be7127a41501fc4fb6dd89eaaf651b11c35bdf4af9", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:71f9cd4acb48aaa9edb483403f56ba5add68d6fd16d4163146dd17b90022dc75";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "main", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:ccfe459a63650eda00ef753c17833baf8b18fa00d94498e7188892b811da6bad", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:5d3fc02aac40525448cf6ff95e6189d0f73d2fac3ef3b127e301692f8a3446d5";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "mmproj", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:8c803cfd62e682e850be111651d6c48638acde40d613e8731cd801af0a5e6609", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:b47d5a8143d26d8310762a57c4c77831885997607541e76d170f90accfc8520e";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:30cce712f6d78a4b9929118088238fa60f73cae59366d72bb59ffaa0dce062e2", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:c623fae9e4cd00f515cbec2a96ff2b34dc545891c3bbca713cc4c4bbb2e4f9ed", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:3f931226247f4232e1935eeca27c0357d2158a451cc487f8363f64d766b354e5", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:876427490cea9c4a4092b8ee267004aa4a870473001b535eeccbdecad35b7bb4", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:f85c07170d4e2e69712b451a61035e8903630013f8e2620ff40e4d84e3e3ee5e", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:716d481cc4904e7fcaea0f4d88dcdb82c1ec12cce4549bb84328f23b8889edb8", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
            slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:0f8ec49590d1588bc4e64da6a0d7f41a343cb81fced9a4d15debe79fcbf9fb67", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:7ad77aca82c3599093fdb2a79d0ce23dd4fa13cfbbdb48466b6b0b59df9d8002", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:80b75293d4e2f26635a12b6cd0dde198f08caae2966246fb1b08c6f8b8002e39", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.streaming", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:eed75103e7204750539ef40f755f9657e2443de1ed520c3ef754f984845a7d0c", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:a7405961c2d5efa6d4f9a6e21521f04f8dcac0b7db902d46f9f3a221846b7a32";
            slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        leaf.owners = {
            {"chat.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"chat.buffered\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming", "slot-action.buffered"}},
            {"completion.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"completion.buffered\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.streaming", "slot-action.buffered"}},
            {"responses.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"responses.buffered\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.streaming", "slot-action.buffered"}},
            {"chat.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"chat.streaming\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "slot-action.buffered"}},
            {"completion.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"completion.streaming\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.buffered", "slot-action.buffered"}},
            {"responses.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"responses.streaming\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.tool_schema_limits"}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.buffered", "slot-action.buffered"}},
            {"embedding.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"embedding.buffered\",\"model_modes\":[\"*\"],\"operation\":\"embeddings\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/embeddings\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"reranking.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"reranking.buffered\",\"model_modes\":[\"*\"],\"operation\":\"reranking\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/rerank\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"slots.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"slots.buffered\",\"model_modes\":[\"*\"],\"operation\":\"slots\",\"transport\":{\"kind\":\"http_json\",\"method\":\"GET\",\"path\":\"/slots\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"slot-action.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"slot-action.buffered\",\"model_modes\":[\"*\"],\"operation\":\"slot_action\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/slots/{slot_id}?action={action}\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "completion.buffered", "responses.buffered", "chat.streaming", "completion.streaming", "responses.streaming", "embedding.buffered", "reranking.buffered", "slots.buffered", "tokenize.buffered"}},
            {"tokenize.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"tokenize.buffered\",\"model_modes\":[\"*\"],\"operation\":\"tokenize\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/tokenize\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
        };
        leaf.owner_graph_digest = "sha256:6bc3f2b40d73e44ceb384564fbc403b1f58cf22c91de863f0746c5f22713e0f8";
        policies.leaves.push_back(std::move(leaf));
        policies.execution_owners.push_back(reviewed_container_execution_owner("rocmfpx"));
    }
    {
        BuiltinExportContract leaf{{"halogen", 1}, "sha256:16046f47a5a52279f1756160e45c8e57c0d8df0332d85ff47a609ad78aae72a8",
            "sha256:f7e734ca8393241999fd14f9251e140d5dc2e9c0babbc341dde84f46f208e8a7", "sha256:0af4c182d17c92a161b6ecbe40822533b9b56277ebd5660486e4f64c7377798c",
            1, "sha256:d1fffec8c948395d3b3dcb973e34bbc60aadf33b52efee7f21788555076643ae", {}};
        leaf.owners = {
            {"chat.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"chat.buffered\",\"model_modes\":[\"chat\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming"}},
            {"chat.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"chat.streaming\",\"model_modes\":[\"chat\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered"}},
            {"completion.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"completion.streaming\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {}},
            {"responses.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"responses.streaming\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {}},
        };
        leaf.owner_graph_digest = "sha256:a767f334f69ddb159002533e978a8623117e8982e53e1c2a95e3cdabfa1cae5d";
        policies.leaves.push_back(std::move(leaf));
        policies.execution_owners.push_back(reviewed_container_execution_owner("halogen"));
    }
    {
        BuiltinExportContract leaf{{"halobox", 1}, "sha256:8fd8236e9178e1ce02a4a0d222f1c40297704fed9f518a555e9d23cb0a70ab19",
            "sha256:bfd48a2318fdaf843d294015fde6f83a8c5ad8d31f116365a8381f70ae02a3b3", "sha256:ac5237abf4c9938fbe0758fc63e1b4b72e00464ae1ce3b9c786b1fcfd4cb8224",
            1, "sha256:830daecf40384f9ab2774bd913bdcf62ef14a375cde342343f6d145082530794", {}};
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Root, "backend", "backend", "llamacpp.backend.v1"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:469621a20ab81e5ef9e7a4f01d62664edbe4437bfd62c415c3edcf2158b9e9bb", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:ad7808ae0f3ae836c7816b8d043a9d06582679daee2af6a1d8e0e94f3854f15b";
            slot.ordered_hooks = {"llamacpp.backend.v1"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Root, "launch", "launch", "llamacpp.prepare.v1"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:85976826dd992792a70403b2360f464ad216e311b74c68d924cad844c4154e54", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:34554e16d632b4823d1f9bd93a6cd5b275ae8a342d236dbc60b862f5dd10c1f6";
            slot.ordered_hooks = {"llamacpp.prepare.v1"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Root, "runtime_options", "runtime_options", "llamacpp.runtime_options.v1"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:b91dd6fcb19cc65819ca70c67ee7a048d7e727e85ec179a80ccc59138a100567", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:0b25f5b68b2e1fcb6c12b544cfdbad2febd614bef14f1da43458b9144cb18e8a";
            slot.ordered_hooks = {"llamacpp.runtime_options.v1"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "draft", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:ebd93e189e5f390e7068ae0eafa76a2918bb55540ffdef4060e8ae720fa40c99", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:0df8b805d831ae8e6dd5728afe30b9a4e7a5d64edc627da729999581f379fa22";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "main", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:194b718f5a26143e893972cf863754d3bf58d058f5723d65e12476488b8d4f63", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e7ae4dde49a12f08e1f40474006261c4dff420e71b782ca7b12b3bd9688535f9";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "mmproj", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:ddc4852581688c430b75cbcda70ab76d79ec24d72cc16070f71532f56b18a198", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:48ffcce1dfe8293e6d050bec08815fabdb5261105f597db4a4be44e22c72bb8c";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:30cce712f6d78a4b9929118088238fa60f73cae59366d72bb59ffaa0dce062e2", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:c623fae9e4cd00f515cbec2a96ff2b34dc545891c3bbca713cc4c4bbb2e4f9ed", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:3f931226247f4232e1935eeca27c0357d2158a451cc487f8363f64d766b354e5", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:876427490cea9c4a4092b8ee267004aa4a870473001b535eeccbdecad35b7bb4", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:f85c07170d4e2e69712b451a61035e8903630013f8e2620ff40e4d84e3e3ee5e", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:716d481cc4904e7fcaea0f4d88dcdb82c1ec12cce4549bb84328f23b8889edb8", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
            slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:0f8ec49590d1588bc4e64da6a0d7f41a343cb81fced9a4d15debe79fcbf9fb67", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:7ad77aca82c3599093fdb2a79d0ce23dd4fa13cfbbdb48466b6b0b59df9d8002", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:80b75293d4e2f26635a12b6cd0dde198f08caae2966246fb1b08c6f8b8002e39", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.streaming", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:eed75103e7204750539ef40f755f9657e2443de1ed520c3ef754f984845a7d0c", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:a7405961c2d5efa6d4f9a6e21521f04f8dcac0b7db902d46f9f3a221846b7a32";
            slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        leaf.owners = {
            {"chat.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"chat.buffered\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming", "slot-action.buffered"}},
            {"completion.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"completion.buffered\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.streaming", "slot-action.buffered"}},
            {"responses.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"responses.buffered\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.streaming", "slot-action.buffered"}},
            {"chat.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"chat.streaming\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "slot-action.buffered"}},
            {"completion.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"completion.streaming\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.buffered", "slot-action.buffered"}},
            {"responses.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"responses.streaming\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.tool_schema_limits"}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.buffered", "slot-action.buffered"}},
            {"embedding.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"embedding.buffered\",\"model_modes\":[\"*\"],\"operation\":\"embeddings\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/embeddings\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"reranking.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"reranking.buffered\",\"model_modes\":[\"*\"],\"operation\":\"reranking\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/rerank\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"slots.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"slots.buffered\",\"model_modes\":[\"*\"],\"operation\":\"slots\",\"transport\":{\"kind\":\"http_json\",\"method\":\"GET\",\"path\":\"/slots\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"slot-action.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"slot-action.buffered\",\"model_modes\":[\"*\"],\"operation\":\"slot_action\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/slots/{slot_id}?action={action}\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "completion.buffered", "responses.buffered", "chat.streaming", "completion.streaming", "responses.streaming", "embedding.buffered", "reranking.buffered", "slots.buffered", "tokenize.buffered"}},
            {"tokenize.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"tokenize.buffered\",\"model_modes\":[\"*\"],\"operation\":\"tokenize\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/tokenize\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
        };
        leaf.owner_graph_digest = "sha256:6bc3f2b40d73e44ceb384564fbc403b1f58cf22c91de863f0746c5f22713e0f8";
        policies.leaves.push_back(std::move(leaf));
        policies.execution_owners.push_back(reviewed_container_execution_owner("halobox"));
    }
    {
        BuiltinExportContract leaf{{"nathanw", 1}, "sha256:0793eb267edee800be216f289efcee1b325cb678f1df6c94465262b6740ee309",
            "sha256:95dc682bac51504f6cea4573323dc506ba565aab4c428a570082770b8c99c9a1", "sha256:1439903409c6d8d3f2cf87e547caec3b89de3a430f876d5ff169699c45e375e5",
            1, "sha256:54ff6e8540cc29f4c8b0eff561da873d2598578ecd39a58976f64fc9bc2da1c5", {}};
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Root, "backend", "backend", "llamacpp.backend.v1"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:469621a20ab81e5ef9e7a4f01d62664edbe4437bfd62c415c3edcf2158b9e9bb", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:ad7808ae0f3ae836c7816b8d043a9d06582679daee2af6a1d8e0e94f3854f15b";
            slot.ordered_hooks = {"llamacpp.backend.v1"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Root, "launch", "launch", "llamacpp.prepare.v1"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:85976826dd992792a70403b2360f464ad216e311b74c68d924cad844c4154e54", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:34554e16d632b4823d1f9bd93a6cd5b275ae8a342d236dbc60b862f5dd10c1f6";
            slot.ordered_hooks = {"llamacpp.prepare.v1"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Root, "runtime_options", "runtime_options", "llamacpp.runtime_options.v1"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:b91dd6fcb19cc65819ca70c67ee7a048d7e727e85ec179a80ccc59138a100567", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:0b25f5b68b2e1fcb6c12b544cfdbad2febd614bef14f1da43458b9144cb18e8a";
            slot.ordered_hooks = {"llamacpp.runtime_options.v1"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "draft", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:ebd93e189e5f390e7068ae0eafa76a2918bb55540ffdef4060e8ae720fa40c99", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:0df8b805d831ae8e6dd5728afe30b9a4e7a5d64edc627da729999581f379fa22";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "main", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:194b718f5a26143e893972cf863754d3bf58d058f5723d65e12476488b8d4f63", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e7ae4dde49a12f08e1f40474006261c4dff420e71b782ca7b12b3bd9688535f9";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "mmproj", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:ddc4852581688c430b75cbcda70ab76d79ec24d72cc16070f71532f56b18a198", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:48ffcce1dfe8293e6d050bec08815fabdb5261105f597db4a4be44e22c72bb8c";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:30cce712f6d78a4b9929118088238fa60f73cae59366d72bb59ffaa0dce062e2", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:c623fae9e4cd00f515cbec2a96ff2b34dc545891c3bbca713cc4c4bbb2e4f9ed", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:3f931226247f4232e1935eeca27c0357d2158a451cc487f8363f64d766b354e5", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:4096ce530b3ef090c74098dcf5d824062e3030c611f4e30e4d8450626e10ee30";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:876427490cea9c4a4092b8ee267004aa4a870473001b535eeccbdecad35b7bb4", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "completion.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:f85c07170d4e2e69712b451a61035e8903630013f8e2620ff40e4d84e3e3ee5e", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:33f584bcb73c39f3a8f66ee31f54c3545dd395719833a030053cc1fa46beeeee";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:716d481cc4904e7fcaea0f4d88dcdb82c1ec12cce4549bb84328f23b8889edb8", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
            slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.buffered", "response", "llamacpp.client_model"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:0f8ec49590d1588bc4e64da6a0d7f41a343cb81fced9a4d15debe79fcbf9fb67", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:c0a0fe187b764038eba1484ba530aafc68ce9df03bdd4c87b724f85973367e03";
            slot.ordered_hooks = {"llamacpp.client_model"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.max_tokens_alias"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:7ad77aca82c3599093fdb2a79d0ce23dd4fa13cfbbdb48466b6b0b59df9d8002", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "chat.streaming", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:80b75293d4e2f26635a12b6cd0dde198f08caae2966246fb1b08c6f8b8002e39", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:e29d00593b1a9a81061fd31530664bf0638a5f2acb64f6d5a22b704916710966";
            slot.ordered_hooks = {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Operation, "responses.streaming", "request", "llamacpp.tool_schema_limits"},
                1, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:eed75103e7204750539ef40f755f9657e2443de1ed520c3ef754f984845a7d0c", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:a7405961c2d5efa6d4f9a6e21521f04f8dcac0b7db902d46f9f3a221846b7a32";
            slot.ordered_hooks = {"llamacpp.tool_schema_limits"};
            slot.effects = {HookEffect::Pure};
            leaf.slots.push_back(std::move(slot));
        }
        leaf.owners = {
            {"chat.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"chat.buffered\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming", "slot-action.buffered"}},
            {"completion.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"completion.buffered\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.streaming", "slot-action.buffered"}},
            {"responses.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"responses.buffered\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.tool_schema_limits"}, {"llamacpp.client_model"}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.streaming", "slot-action.buffered"}},
            {"chat.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"chat.streaming\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "slot-action.buffered"}},
            {"completion.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"completion.streaming\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.buffered", "slot-action.buffered"}},
            {"responses.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"responses.streaming\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {"llamacpp.tool_schema_limits"}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.buffered", "slot-action.buffered"}},
            {"embedding.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"embedding.buffered\",\"model_modes\":[\"*\"],\"operation\":\"embeddings\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/embeddings\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"reranking.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"reranking.buffered\",\"model_modes\":[\"*\"],\"operation\":\"reranking\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/rerank\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"slots.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"slots.buffered\",\"model_modes\":[\"*\"],\"operation\":\"slots\",\"transport\":{\"kind\":\"http_json\",\"method\":\"GET\",\"path\":\"/slots\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
            {"slot-action.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"slot-action.buffered\",\"model_modes\":[\"*\"],\"operation\":\"slot_action\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/slots/{slot_id}?action={action}\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered", "completion.buffered", "responses.buffered", "chat.streaming", "completion.streaming", "responses.streaming", "embedding.buffered", "reranking.buffered", "slots.buffered", "tokenize.buffered"}},
            {"tokenize.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"tokenize.buffered\",\"model_modes\":[\"*\"],\"operation\":\"tokenize\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/tokenize\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"slot-action.buffered"}},
        };
        leaf.owner_graph_digest = "sha256:6bc3f2b40d73e44ceb384564fbc403b1f58cf22c91de863f0746c5f22713e0f8";
        policies.leaves.push_back(std::move(leaf));
        policies.execution_owners.push_back(reviewed_container_execution_owner("nathanw"));
    }
    {
        BuiltinExportContract leaf{{"ds4_rocm", 1}, "sha256:df2e01ca09a3c07109bd1d896af7f68071a2596f945a8a3675fc9fac56c43b63",
            "sha256:b0516ff9086535daa40e65930388156f7dee518459889bc12f228a98836b8529", "sha256:69d7e3ca064f3838a656b68bc37821735561fbada1d26da7317bb3a9c3b51cb4",
            1, "sha256:7a1a6b6bf73ba6ef8849ecaa381f3d19830acf888727de759f7ea7cd7972f656", {}};
        {
            ExportSlotPolicy slot{{SlotOwnerKind::Artifact, "main", "artifacts", "llamacpp.artifacts.v1"},
                2, "sha256:6d693d6f53d8a4cf83131c975c7332b76757260037939107fb0d48576143e5e3", "sha256:b611369183da550506ca834798d8fc91b0a42e8a9241439b18fb7ce8f856ee84", SlotRemoval::Independent, {}};
            slot.owner_semantics_digest = "sha256:08537a35212d443664edf6224eb19c28bf09680044fe917249684780f200d778";
            slot.ordered_hooks = {"llamacpp.artifacts.v1"};
            slot.effects = {HookEffect::ManagedInstallRead};
            leaf.slots.push_back(std::move(slot));
        }
        leaf.owners = {
            {"chat.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"chat.buffered\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.streaming"}},
            {"chat.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"chat.streaming\",\"model_modes\":[\"*\"],\"operation\":\"chat_completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/chat/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"chat.buffered"}},
            {"completion.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"completion.buffered\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.streaming"}},
            {"completion.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"completion.streaming\",\"model_modes\":[\"*\"],\"operation\":\"completion\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/completions\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"completion.buffered"}},
            {"responses.buffered", nlohmann::json::parse("{\"delivery\":\"buffered\",\"id\":\"responses.buffered\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_json\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"response_codec\":\"json.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.streaming"}},
            {"responses.streaming", nlohmann::json::parse("{\"delivery\":\"streaming\",\"id\":\"responses.streaming\",\"model_modes\":[\"*\"],\"operation\":\"responses\",\"transport\":{\"kind\":\"http_sse\",\"method\":\"POST\",\"path\":\"/v1/responses\",\"port\":\"http\",\"stream_codec\":\"openai_sse.v1\",\"timeout\":\"global\"}}"),
                {}, {}, {}, false, {},
                {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"}, {"responses.buffered"}},
        };
        leaf.owner_graph_digest = "sha256:d92b0ebdd99222a23be96ef492a3f79159a2ef4b4a5e44fd8d32cf5687a2adbf";
        policies.leaves.push_back(std::move(leaf));
        policies.execution_owners.push_back(reviewed_container_execution_owner("ds4_rocm"));
    }
    policies.digest = "sha256:c754cbcaf5063e97f2e803d8abf2cb2a60f241067077d1f4364b67d5df5a1548";
}

}
