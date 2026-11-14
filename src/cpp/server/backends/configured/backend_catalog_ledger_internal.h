#pragma once

#include "lemon/backends/configured/backend_catalog_execution.h"

#include <map>

namespace lemon::backends::accounting {

class CatalogLedger {
public:
    explicit CatalogLedger(CatalogExecutionLimits limits);
    const CatalogExecutionLimits& limits() const { return limits_; }
    const CatalogExecutionMetrics& metrics() const { return metrics_; }
    void owned(std::size_t bytes);
    void work(std::size_t bytes, std::size_t visits);
    void temporary(std::size_t bytes);
    void release_temporary(std::size_t bytes) noexcept;
    void receipt(const void* object);
    void dependency(const void* object);
    void seal(std::size_t expected, std::size_t finalized);
    bool sealed() const { return sealed_; }
private:
    CatalogExecutionLimits limits_;
    CatalogExecutionMetrics metrics_;
    std::map<const void*, bool> receipts_;
    std::size_t temporary_ = 0;
    bool sealed_ = false;
};

class StageScope {
public:
    StageScope(CatalogLedger& ledger, CatalogBuildStage stage);
    ~StageScope();
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;
private:
    CatalogLedger* previous_;
    CatalogBuildStage previous_stage_;
};

class Temporary {
public:
    explicit Temporary(std::size_t bytes);
    ~Temporary();
    Temporary(const Temporary&) = delete;
    Temporary& operator=(const Temporary&) = delete;
private:
    CatalogLedger* ledger_;
    std::size_t bytes_;
};

void owned(std::size_t bytes);
void work(std::size_t bytes, std::size_t visits = 1);
void dependency(const void* object);
void receipt(const void* object);

}
