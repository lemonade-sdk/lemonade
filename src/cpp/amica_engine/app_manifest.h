#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace amica_engine {

// Derives the amica_app.json that the AMICA ChatApp (PrepareAmicaApps.ps1)
// writes for a bundle, from the bundle's graphs, weights and config files, so a
// bundle runs without being prepared first. Only the fields the engine reads are
// produced. An LM-head graph that has to be adapted to the decoder's output type
// is written to a per-bundle directory under the system temp directory instead
// of into the bundle.
nlohmann::json derive_app_manifest(const std::string& model_dir);

// The LM-head graph text adapted to take the decoder's output type; text is
// returned unchanged when the types already match.
std::string adapt_lm_head(const std::string& text, const std::string& producer_type, const std::string& path);

}  // namespace amica_engine
