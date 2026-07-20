#pragma once

#include <map>
#include <set>
#include <string>

namespace lemon {

// Device constraints: device_type -> set of allowed families (empty = all families)
using DeviceConstraints = std::map<std::string, std::set<std::string>>;

// Optional per-arch restriction, tighter than the owning support row. Used when
// a single family/ISA is installable on only a subset of the row's OSes or ROCm
// channels because the upstream assets for the others aren't published yet
// (e.g. gfx950 / MI350: only the Linux + stable asset exists so far). An arch
// absent from the row's gate map inherits the row's full OS/channel reach.
struct ArchInstallGate {
    std::set<std::string> os;        // if non-empty, arch installable only on these OSes
    std::set<std::string> channels;  // if non-empty, arch installable only on these ROCm channels
};

// arch token (as written in the support row, e.g. "gfx950") -> its extra gate.
using ArchInstallGates = std::map<std::string, ArchInstallGate>;

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
    ArchInstallGates arch_gates;
};

// A backend descriptor's support row, without the recipe (it's always the
// owning descriptor's recipe — assembling a RecipeBackendDef fills it in). Keeps
// the descriptor literals from repeating their own recipe on every row.
struct BackendSupport {
    std::string backend;
    std::set<std::string> supported_os;
    DeviceConstraints devices;
    std::string device_summary = "";
    ArchInstallGates arch_gates;
};

} // namespace lemon
