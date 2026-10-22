#pragma once

#include "lemon/backends/configured/backend_definition_binding.h"

#include <array>

namespace lemon::backends {

class BackendCatalog;

enum class CatalogBuildStage { Admission, Compilation, Binding, Entries, Count };

struct CatalogStageMetrics {
    std::size_t owned_bytes = 0;
    std::size_t work_bytes = 0;
    std::size_t work_visits = 0;
    std::size_t peak_temporary_bytes = 0;
};

struct CatalogExecutionMetrics {
    std::size_t retained_bytes = 0;
    std::size_t peak_temporary_bytes = 0;
    std::size_t peak_bytes = 0;
    std::size_t work_bytes = 0;
    std::size_t work_visits = 0;
    std::size_t dependency_references = 0;
    std::array<CatalogStageMetrics, 4> stages{};
};

struct CatalogExecutionLimits {
    DefinitionLimits admission;
    ExecutionCompileLimits compilation;
    ExecutionBindLimits binding;
    std::size_t retained_bytes = 128 * 1024 * 1024;
    std::size_t temporary_bytes = 128 * 1024 * 1024;
    std::size_t work_bytes = 64 * 1024 * 1024;
    std::size_t work_visits = 4 * 1048576;
    std::size_t diagnostics = 100;
    std::array<CatalogStageMetrics, 4> stages{{
        {128 * 1024 * 1024, 64 * 1024 * 1024, 4 * 1048576, 128 * 1024 * 1024},
        {128 * 1024 * 1024, 64 * 1024 * 1024, 4 * 1048576, 128 * 1024 * 1024},
        {128 * 1024 * 1024, 64 * 1024 * 1024, 4 * 1048576, 128 * 1024 * 1024},
        {128 * 1024 * 1024, 64 * 1024 * 1024, 4 * 1048576, 128 * 1024 * 1024}}};
};

struct CatalogBuildStages {
    bool resolved = false;
    bool authorized = false;
    bool catalog_admitted = false;
    bool compiled = false;
    bool bound = false;
    bool executable = false;
};

struct CatalogBuildResult {
    std::shared_ptr<const BackendCatalog> catalog;
    CompositionDiagnostics diagnostics;
    CatalogBuildStages stages;
    CatalogExecutionMetrics metrics;
};

CatalogBuildResult build_backend_catalog(std::shared_ptr<const FrozenBuiltinCatalog> builtins,
    const std::vector<ExternalDefinitionInput>& sources, const CatalogExecutionLimits& limits = {});

}
