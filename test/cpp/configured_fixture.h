#pragma once

#include "backend_definition_resources.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/launch_planner.h"
#include "lemon/utils/aixlog.hpp"
#include "thinksound_oracle.h"

namespace thinksound_oracle {

struct LogCapture {
    std::vector<std::string> records;
    explicit LogCapture(const std::string& tag) {
        AixLog::Log::init<AixLog::SinkCallback>(AixLog::Severity::trace, [this, tag](const AixLog::Metadata& metadata, const std::string& message) {
            if (metadata.tag.text == tag) records.push_back(AixLog::to_string(metadata.severity) + " " + message);
        });
    }
    ~LogCapture() { AixLog::Log::init({}); }
};

inline std::shared_ptr<const backends::ConfiguredBackendEntry> entry(const std::string& text = backends::k_thinksound_definition, backends::TrustedCompatibility compatibility = backends::TrustedCompatibility::None) {
    const auto result = backends::BackendDefinitionStore().validate({"fixture.json", text, backends::DefinitionOrigin::Builtin});
    if (!result.definition) {
        for (const auto& error : result.diagnostics) std::printf("%s %s\n", error.code.c_str(), error.pointer.c_str());
        throw std::runtime_error("Invalid configured fixture");
    }
    return backends::ConfiguredBackendEntry::make(result.definition, compatibility);
}
class FixtureLaunchServices : public backends::EffectLaunchServices {
public:
    explicit FixtureLaunchServices(EffectsFixture& fixture) : backends::EffectLaunchServices(fixture.effects()), effects_(fixture.effects()), external_(fixture.external) {}
    bool exists(const std::string& path) const override {
        return path == external_ && !external_.empty() ? effects_.binary.exists(path) : effects_.exists(path);
    }
private:
    backends::BackendEffects effects_;
    std::string external_;
};

}
