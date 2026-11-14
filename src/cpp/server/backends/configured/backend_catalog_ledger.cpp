#include "backend_catalog_ledger_internal.h"
#include "backend_definition_v2_internal.h"

#include <algorithm>

namespace lemon::backends::accounting {
namespace {
thread_local CatalogLedger* active = nullptr;
thread_local CatalogBuildStage stage = CatalogBuildStage::Admission;
std::size_t index() { return static_cast<std::size_t>(stage); }
void bound(std::size_t used, std::size_t bytes, std::size_t limit) {
    if (used > limit || bytes > limit - used) throw v2::LimitExceeded();
}
}

CatalogLedger::CatalogLedger(CatalogExecutionLimits limits) : limits_(std::move(limits)) {
    const CatalogExecutionLimits hard;
    limits_.retained_bytes = std::min(limits_.retained_bytes, hard.retained_bytes);
    limits_.temporary_bytes = std::min(limits_.temporary_bytes, hard.temporary_bytes);
    limits_.work_bytes = std::min(limits_.work_bytes, hard.work_bytes);
    limits_.work_visits = std::min(limits_.work_visits, hard.work_visits);
    limits_.diagnostics = std::min(limits_.diagnostics, hard.diagnostics);
    for (std::size_t i = 0; i < limits_.stages.size(); ++i) {
        auto& value = limits_.stages[i];
        const auto& maximum = hard.stages[i];
        value.owned_bytes = std::min(value.owned_bytes, maximum.owned_bytes);
        value.work_bytes = std::min(value.work_bytes, maximum.work_bytes);
        value.work_visits = std::min(value.work_visits, maximum.work_visits);
        value.peak_temporary_bytes = std::min(value.peak_temporary_bytes, maximum.peak_temporary_bytes);
    }
}
void CatalogLedger::owned(std::size_t bytes) {
    if (sealed_) throw std::logic_error("Catalog ledger is sealed");
    auto& current = metrics_.stages[index()];
    bound(metrics_.retained_bytes, bytes, limits_.retained_bytes);
    bound(metrics_.retained_bytes + bytes, temporary_, limits_.retained_bytes);
    bound(current.owned_bytes, bytes, limits_.stages[index()].owned_bytes);
    metrics_.retained_bytes += bytes;
    current.owned_bytes += bytes;
    metrics_.peak_bytes = std::max(metrics_.peak_bytes, metrics_.retained_bytes + temporary_);
}
void CatalogLedger::work(std::size_t bytes, std::size_t visits) {
    if (sealed_) throw std::logic_error("Catalog ledger is sealed");
    auto& current = metrics_.stages[index()];
    const auto& limit = limits_.stages[index()];
    bound(metrics_.work_bytes, bytes, limits_.work_bytes);
    bound(metrics_.work_visits, visits, limits_.work_visits);
    bound(current.work_bytes, bytes, limit.work_bytes);
    bound(current.work_visits, visits, limit.work_visits);
    metrics_.work_bytes += bytes;
    metrics_.work_visits += visits;
    current.work_bytes += bytes;
    current.work_visits += visits;
}
void CatalogLedger::temporary(std::size_t bytes) {
    if (sealed_) throw std::logic_error("Catalog ledger is sealed");
    bound(temporary_, bytes, limits_.temporary_bytes);
    bound(temporary_, bytes, limits_.stages[index()].peak_temporary_bytes);
    bound(metrics_.retained_bytes, temporary_ + bytes, limits_.retained_bytes);
    temporary_ += bytes;
    metrics_.peak_temporary_bytes = std::max(metrics_.peak_temporary_bytes, temporary_);
    metrics_.stages[index()].peak_temporary_bytes = std::max(metrics_.stages[index()].peak_temporary_bytes, temporary_);
    metrics_.peak_bytes = std::max(metrics_.peak_bytes, metrics_.retained_bytes + temporary_);
}
void CatalogLedger::release_temporary(std::size_t bytes) noexcept { temporary_ -= bytes; }
void CatalogLedger::receipt(const void* object) {
    if (!object || receipts_.count(object)) throw std::logic_error("Invalid catalog ownership receipt");
    owned(sizeof(decltype(receipts_)::value_type) + 64);
    receipts_.emplace(object, true);
}
void CatalogLedger::dependency(const void* object) {
    if (!object || !receipts_.count(object)) throw std::logic_error("Foreign catalog dependency");
    work(0, 1);
    ++metrics_.dependency_references;
}
void CatalogLedger::seal(std::size_t expected, std::size_t finalized) {
    if (sealed_ || temporary_ || expected != finalized) throw std::logic_error("Incomplete catalog finalization");
    sealed_ = true;
}
StageScope::StageScope(CatalogLedger& ledger, CatalogBuildStage selected)
    : previous_(active), previous_stage_(stage) { active = &ledger; stage = selected; }
StageScope::~StageScope() { active = previous_; stage = previous_stage_; }
Temporary::Temporary(std::size_t bytes) : ledger_(active), bytes_(bytes) { if (ledger_) ledger_->temporary(bytes); }
Temporary::~Temporary() { if (ledger_) ledger_->release_temporary(bytes_); }
void owned(std::size_t bytes) { if (active) active->owned(bytes); }
void work(std::size_t bytes, std::size_t visits) { if (active) active->work(bytes, visits); }
void dependency(const void* object) { if (active) active->dependency(object); }
void receipt(const void* object) { if (active) active->receipt(object); }

}
