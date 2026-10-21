#pragma once

#include "lemon/backends/configured/backend_definition_authorization.h"
#include "lemon/backends/configured/backend_definition_exports.h"

namespace lemon::backends {

struct SealedHookGrant {
    std::string base_id;
    std::uint32_t contract_major;
    std::string source_digest;
    std::string definition_hash;
    std::string dependency_digest;
    LogicalHookSlot slot;
    std::uint32_t abi_revision;
    std::string parameter_digest;
    std::string mutation_policy_digest;
};

struct AuthorityInspection {
#ifdef LEMON_BACKEND_EXPORT_TESTING
    static std::shared_ptr<const AuthorizedDefinition> test_without_grants(const AuthorizedDefinition& token);
#endif
    static std::size_t policy_count(const AuthorizedDefinition& token) noexcept;
    static std::size_t grant_count(const AuthorizedDefinition& token) noexcept;
    static const SealedHookGrant& grant(const AuthorizedDefinition& token, std::size_t ordinal);
    static const ExportSlotPolicy& policy(const AuthorizedDefinition& token, std::size_t ordinal);
};

}
