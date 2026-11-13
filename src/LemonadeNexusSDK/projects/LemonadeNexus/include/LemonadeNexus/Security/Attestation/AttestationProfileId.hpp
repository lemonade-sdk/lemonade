#pragma once

// Compiled identity of one attestation profile.
//
// The profile ID names WHICH evidence format and provider examine a candidate.
// The profile ruleset names WHICH VERSION of that provider's rules apply. Both
// bind into the challenge and into the signed evidence digest, so a candidate
// cannot answer a challenge issued for one profile with evidence built for
// another, and cannot ask for a weaker profile when its own fails.
//
// The verified binary carries the whole set. There is no loader, no negotiation
// and no configuration switch: an unknown ID has no provider and fails closed.
//
// Architecture reference: Security Architecture Final Draft 1.1, sections 5.4
// and 31.

#include <cstdint>
#include <string_view>

namespace nexus::security {

/// How much IMA history a challenge requests. Lives below AttestationTypes.hpp
/// because PlatformEvidence, embedded by AttestationEvidence, needs it.
enum class ImaEvidenceMode : uint16_t {
    Full = 1,
    Resume = 2,
};

enum class AttestationProfileId : uint16_t {
    /// No profile. A default-constructed challenge or bundle lands here, so it
    /// fails closed instead of defaulting into a real profile.
    Unknown = 0,

    /// AMD SEV-SNP with a vTPM bound through an HCL runtime-data blob: the
    /// paravisor shape. The one profile implemented today.
    AzureSnpVtpm = 1,

    /// AMD SEV-SNP with a COCONUT-SVSM vTPM at VMPL0, bound to a Linux guest
    /// report above VMPL0 and an enrolled AK through credential activation.
    SnpSvsmVtpm = 2,

    /// AMD SEV-SNP direct measured boot, no TPM. Cannot reach Tier 1: it has
    /// no protected runtime measurement accumulator (1.1 section 7).
    SnpDirectBoot = 3,

    /// Intel TDX. Named so the enum stays stable; no provider exists.
    Tdx = 4,
};

using AttestationProfileRuleset = uint16_t;

/// Ruleset version shared by every compiled profile. A change to what any
/// provider requires is a bump here, and a bump invalidates every outstanding
/// challenge rather than letting old evidence answer new rules.
// Ruleset 3: verifier-owned Full/Resume IMA checkpoints and a required,
// durably verified EK/AK activation record.
inline constexpr AttestationProfileRuleset kAttestationProfileRulesetVersion = 3;

/// The profile this binary issues challenges under, and the one its own prover
/// answers with. The verified binary picks it; no host input reaches this
/// value. A release switches this only after its selected provider and pinned
/// profile have passed qualification.
inline constexpr AttestationProfileId kTier1AttestationProfileId =
    AttestationProfileId::SnpSvsmVtpm;

[[nodiscard]] std::string_view attestation_profile_id_name(AttestationProfileId id);

/// True for an ID this binary names. A named ID may still have no provider,
/// or a provider that cannot yet verify anything.
[[nodiscard]] bool is_known_attestation_profile_id(AttestationProfileId id);

}  // namespace nexus::security
