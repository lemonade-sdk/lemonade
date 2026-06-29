#pragma once

#include <map>
#include <set>
#include <string>

namespace lemon {

// Device constraints: device_type -> set of allowed families (empty = all families)
using DeviceConstraints = std::map<std::string, std::set<std::string>>;

// A single recipe/backend support row: which OS and device families a given
// (recipe, backend) pair runs on. The canonical support matrix is assembled by
// collecting these rows from every backend descriptor (see BackendDescriptor::support).
//
// IMPORTANT: For recipes with multiple backends (e.g. llamacpp), the order in
// which these rows appear defines the preference order — first listed = most
// preferred. Empty family set {} means "all families of that device type".
struct RecipeBackendDef {
    std::string recipe;
    std::string backend;
    std::set<std::string> supported_os;
    DeviceConstraints devices;
    // Human-friendly device description for the generated support matrix (README).
    // May contain footnote markers (e.g. "*") whose text lives as prose in the doc.
    std::string device_summary = "";
};

// A backend descriptor's support row, without the recipe (it's always the
// owning descriptor's recipe — assembling a RecipeBackendDef fills it in). Keeps
// the descriptor literals from repeating their own recipe on every row.
struct BackendSupport {
    std::string backend;
    std::set<std::string> supported_os;
    DeviceConstraints devices;
    std::string device_summary = "";
};

} // namespace lemon
