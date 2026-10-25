#pragma once

// The compiled Nexus security rules.
//
// Every value here is protocol truth, not operator preference. The verified
// binary carries these constants; binary attestation is what protects them.
// Changing a value that alters deterministic security behavior requires a new
// ruleset version and a new verified release — never a configuration file.
//
// Architecture reference: Security Architecture Final Draft 1.1, sections 2,
// 13, 16, 17 and 20.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nexus::security::constants {

// --- Ruleset identity -------------------------------------------------------

// Covers deterministic security behavior: Tier 1 prerequisites, selection
// rules, threshold formulas, DKG rules, and state-transition validity.
inline constexpr uint16_t kSecurityRulesetVersion = 1;

// Covers consensus safety and consensus validity. A safety-rule change MUST
// increment this value.
inline constexpr uint16_t kConsensusRulesetVersion = 1;

inline constexpr uint16_t kBftProtocolVersion = 1;
inline constexpr uint16_t kQcFormatVersion = 1;

// On-disk format of the durable consensus store records
// (hotstuff-safety-<epoch>.json and hotstuff-commit-<epoch>.json). The format
// is new; every write carries this version and every read requires it.
inline constexpr uint32_t kConsensusStoreFormatVersion = 1;

// --- Domain-separation strings ----------------------------------------------

inline constexpr std::string_view kBftProtocolDomain = "LEMONADE-NEXUS-HOTSTUFF-V1";
inline constexpr std::string_view kFrostCiphersuite = "FROST(Ed25519, SHA-512)";
inline constexpr std::string_view kTier1AttestDomain = "LEMONADE-NEXUS-T1-ATTEST-V1";
inline constexpr std::string_view kTier1SelectDomain = "LEMONADE-NEXUS-T1-SELECT-V1";
inline constexpr std::string_view kLeaderOrderDomain = "LEMONADE-NEXUS-HOTSTUFF-LEADER-V1";

// --- Tier 1 population ------------------------------------------------------

inline constexpr std::size_t kBootstrapThreshold = 5;
inline constexpr std::size_t kMinActiveTier1 = 5;
inline constexpr std::size_t kPreferredMinActiveTier1 = 7;
inline constexpr std::size_t kMaxActiveTier1 = 31;
inline constexpr std::size_t kMinTier1Reserve = 2;

struct Tier1TargetStep {
    std::size_t min_admitted;
    std::size_t target;
};

// Compiled step table: admitted mesh servers -> desired active Tier 1 count.
// Below the first step the mesh is pre-quorum and the target is zero.
inline constexpr std::array<Tier1TargetStep, 10> kTier1TargetSteps{{
    {5, 5},
    {10, 7},
    {25, 10},
    {100, 13},
    {250, 16},
    {500, 19},
    {1000, 22},
    {2500, 25},
    {5000, 28},
    {10000, 31},
}};

// --- Quorum formulas --------------------------------------------------------

// f(N) = floor((N - 1) / 3). Zero is not a valid population; treat it as
// zero faults so the size_t subtraction cannot wrap.
[[nodiscard]] constexpr std::size_t max_byzantine_faults(std::size_t n) {
    return n == 0 ? 0 : (n - 1) / 3;
}

[[nodiscard]] constexpr std::size_t consensus_quorum(std::size_t n) {
    return n - max_byzantine_faults(n);
}

// The bootstrap threshold is a floor, never a ceiling: a larger active set
// always raises the authority threshold with it.
[[nodiscard]] constexpr std::size_t authority_threshold(std::size_t n) {
    const std::size_t quorum = consensus_quorum(n);
    return quorum < kBootstrapThreshold ? kBootstrapThreshold : quorum;
}

[[nodiscard]] constexpr std::size_t tier1_target_count(std::size_t admitted) {
    std::size_t target = 0;
    for (const auto& step : kTier1TargetSteps) {
        if (admitted >= step.min_admitted) {
            target = step.target;
        }
    }
    return target;
}

// --- Epoch and attestation cadence ------------------------------------------

inline constexpr uint64_t kTargetEpochSeconds = 3600;
inline constexpr uint64_t kReattestIntervalSeconds = 900;
inline constexpr uint64_t kFinalAttestMaxAgeSeconds = 300;

// How long an authorized next-epoch DKG may sit incomplete before the attempt
// fails and the deterministic replacement runs. Liveness only: a stall never
// activates anything, and the silent participants are excluded from the next
// attempt by their own observed silence. Wide enough that a run of view
// timeouts at the pacemaker's maximum backoff can never masquerade as one.
inline constexpr uint64_t kDkgStallSeconds = 300;

// Tier 1 attestation is expensive; the budget binds to node identity and
// epoch, never to an IP address (architecture 24). Initial value; a tuning
// item.
inline constexpr uint32_t kMaxTier1AttestAttemptsPerEpoch = 4;

// Final-readiness challenges are budgeted PER PLAN ATTEMPT, separately from
// the eligibility cadence: the bucket is keyed by the plan-bound attestation
// context, so a replacement attempt starts fresh and no wall-clock horizon —
// an epoch can legally outlive its target — ever wedges an honest boundary.
// Exhaustion is a protocol signal, not a dead end: the attempt fails, the
// old epoch stays authoritative, and the next deterministic attempt carries
// a fresh bucket. Attempts are consensus-committed plans, so the challenge
// source stays bounded by finality, never by time. The value matches the
// eligibility budget: one initial exchange plus three renewals.
inline constexpr uint32_t kMaxFinalAttestAttemptsPerPlan = kMaxTier1AttestAttemptsPerEpoch;

// Bootstrap retry cadence, offset per node by identity-derived jitter so
// founding-set retries do not land as one synchronized burst.
inline constexpr uint64_t kBootstrapRetryMs = 15000;
inline constexpr uint64_t kBootstrapRetryJitterMs = 5000;

// Re-send cadence for an epoch-transition or adoption DKG in progress. Its
// datagrams are mortal and a package can reach a peer before that peer's
// session exists; re-sending lets every participant finish without waiting
// for the stall window. Receivers treat duplicates as benign.
inline constexpr uint64_t kDkgResendMs = 5000;

// --- Mesh eligibility observations ------------------------------------------

// Distinct attestations one observer must have verified before it counts
// toward continuity. Two rounds, and because an observer only issues a
// challenge every kReattestIntervalSeconds, two of them span at least one
// interval without anyone trusting a peer's clock.
inline constexpr std::size_t kMinContinuityObservations = 2;

// Observations expire at the epoch boundary and affect the NEXT epoch's
// eligibility only. A node stays eligible while the mesh keeps seeing it, never
// because it was healthy once; and nothing here can shrink a frozen epoch.
inline constexpr uint64_t kObservationValidityEpochs = 1;

// Distinct attestations kept per observer per subject. Well above the rounds
// one epoch can hold, so it bounds memory against a flooding observer without
// ever being reached by honest cadence.
inline constexpr std::size_t kMaxContinuityAttestations = 8;

// --- HotStuff safety rules --------------------------------------------------

inline constexpr std::size_t kHotStuffChainLength = 3;

inline constexpr bool kVoteOncePerView = true;
inline constexpr bool kRequireParentQc = true;
inline constexpr bool kRequireSafeNodeRule = true;
inline constexpr bool kLockOnTwoChain = true;
inline constexpr bool kCommitOnThreeChain = true;

inline constexpr bool kEpochMembershipFrozen = true;
inline constexpr bool kAllowWeightedVotes = false;
inline constexpr bool kAllowMixedConsensusRulesets = false;

inline constexpr bool kQcUsesExplicitSignatures = true;
inline constexpr bool kQcDeduplicateByNodeId = true;

inline constexpr uint64_t kConsensusVoteKeyLifetimeEpochs = 1;

inline constexpr bool kMembershipChangesOnlyAtEpochBoundary = true;
inline constexpr bool kConsensusRuleChangesOnlyAtEpochBoundary = true;
inline constexpr bool kCheckpointEveryCommit = true;
inline constexpr bool kSignCanonicalDigestOnly = true;

// --- Pacemaker (liveness only; never part of message validity) --------------

inline constexpr uint64_t kViewTimeoutBaseMs = 2000;
inline constexpr uint64_t kViewTimeoutMaxMs = 30000;
inline constexpr uint64_t kViewTimeoutBackoffFactor = 2;
inline constexpr uint32_t kTimeoutResetAfterCommittedBlocks = 3;

// --- Consensus resource limits (applied before expensive work) --------------

// A block payload (BlockPayload.hpp) carries at most kMaxTransitionsPerBlock
// entries whose canonical encodings total at most kMaxConsensusBlockBytes, so
// the proposal message that carries it stays inside kMaxSecurityMessageBytes
// together with the proposal header, the justify certificate at the largest
// population, and the payload framing. A larger backlog uses more blocks.
inline constexpr std::size_t kMaxConsensusBlockBytes = 48000;
inline constexpr std::size_t kMaxTransitionsPerBlock = 128;
inline constexpr uint64_t kMaxFutureViewDistance = 64;
inline constexpr std::size_t kMaxPendingProposals = 128;
inline constexpr std::size_t kMaxQcSignatures = kMaxActiveTier1;

// --- Security transport bounds (applied before any parse or crypto work) ----

// v2 added the bounded evidence-transfer state machine and the SVSM AK
// activation messages; v3 the release-approval commitment in GenesisFounding
// and the bootstrap certificate.
//
// The optional block payload at the end of a proposal and the TreeOpSubmit
// message are additions, not layout changes: a message without them keeps its
// v3 bytes, and only a node with tree operations on sends them. The
// storage-key messages (kinds 32 to 37) and the StorageUnlock attestation
// purpose are additions of the same sort. The storage key runs on every
// server, so every server sends them; a peer without them drops the kinds it
// does not know.
inline constexpr uint8_t kSecurityWireVersion = 3;
inline constexpr std::size_t kMaxSecurityMessageBytes = 60000;
inline constexpr std::size_t kMaxPlatformEvidenceWireBytes = 56 * 1024;

// Wire bound of one block payload body: the entry bytes plus, per entry, a
// u32 length and a 64-byte signature, plus a u16 count. The envelope limit
// still applies to the whole proposal message.
inline constexpr std::size_t kMaxBlockPayloadBytes =
    kMaxConsensusBlockBytes + kMaxTransitionsPerBlock * (4 + 64) + 2;
static_assert(kMaxBlockPayloadBytes < kMaxSecurityMessageBytes);

// The collector IPC wire version. Authoritative here so the release-approval
// verifier and the attestd codec cannot drift; AttestdCodec.hpp aliases it.
// Version 4 added the SVSM AK activation operation to the evidence socket.
inline constexpr uint16_t kAttestdWireVersion = 4;

// Raw IMA chunk payload cap, leaving over 10 KiB of a mesh envelope for the
// authenticated envelope fields and typed chunk metadata.
inline constexpr std::size_t kMaxEvidenceChunkBytes = 48 * 1024;

// Full carries the fixed platform bundle plus raw IMA: a measured ~2.3 MB log
// with reports, endorsements and metadata fits with >1.7 MiB headroom. Resume
// caps the suffix an ordinary cadence may add before a Full refresh.
inline constexpr std::size_t kMaxFullEvidenceTransferBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxResumeImaSuffixBytes = 512 * 1024;
inline constexpr std::size_t kMaxEvidenceTransferChunks =
    (kMaxFullEvidenceTransferBytes + kMaxEvidenceChunkBytes - 1) /
    kMaxEvidenceChunkBytes;
inline constexpr std::size_t kMaxConcurrentEvidenceTransfers = 8;
inline constexpr uint64_t kEvidenceTransferDeadlineMs = 30000;

// Verifier-owned state is finite even if every observed identity rotates.
inline constexpr std::size_t kMaxAttestationCheckpoints = 128;
inline constexpr std::size_t kMaxSvsmActivationRecords = 128;

// Most components one attestation profile may approve. This is what makes a
// checkpoint summary sizeable from the compiled profile instead of from the
// measurement log: one entry per approved path, and the list is bounded here.
inline constexpr std::size_t kMaxSummarisedPaths = 16;
inline constexpr std::size_t kMaxDkgPayloadBytes = 4096;
inline constexpr std::size_t kMaxFrostPayloadBytes = 1024;
inline constexpr std::size_t kMaxCiphersuiteNameBytes = 64;

// Per-peer message budget: a member cannot drive the router faster than this.
inline constexpr uint32_t kSecurityPeerMessagesPerWindow = 256;
inline constexpr uint64_t kSecurityFloodWindowMs = 1000;
inline constexpr std::size_t kSecurityTrackedPeers = 512;
inline constexpr std::size_t kSecurityDedupeWindow = 4096;

// A single attestation answers one challenge, and one challenger at a time
// has no legitimate reason to force more than a handful of productions.
// Bounded well below the flood budget so a member at 256 msg/s still cannot
// extract one full TPM quote per message.
inline constexpr uint32_t kChallengeProductionsPerWindow = 4;
inline constexpr uint64_t kChallengeProductionWindowMs = 1000;

// The uncommitted chain is at most a few blocks in chained HotStuff; the
// sync reply is bounded well above that and well below the envelope limit.
inline constexpr std::size_t kMaxSyncChainBlocks = 8;
// A running replica further behind than one sync reply fetches the gap in
// pages of kMaxSyncChainBlocks. It asks one peer for the same page again only
// after this interval, so a peer that cannot serve the page is not asked in a
// loop.
inline constexpr uint64_t kSyncPageRetryMs = 1000;

// Handoff-chain links per page. A link at the largest population is under
// 10 KB on the wire, so four keep the page inside the envelope limit with the
// epoch-1 base beside them. A longer history is fetched page by page; nothing
// accepts one attacker-sized chain object.
inline constexpr std::size_t kMaxHandoffChainLinks = 4;

// --- Threshold storage key (design Part B, Phase 5) -------------------------

// The storage key role's domain tag; the storage-threshold crate binds the same
// value into every partial's transcript.
inline constexpr std::string_view kStorageKeyDomain = "LEMONADE-NEXUS-STORAGE-V1";

// Holder counts (B4, B10). The first key needs kStorageMinCreateHolders
// eligible, attested holders. After holder loss Tier 1 re-shares or rotates
// while at least kStorageMinReshareHolders remain; below that the last plan
// stays. kStorageMaxHolders caps the holder set; the crate enforces the same
// three values.
inline constexpr std::size_t kStorageMinCreateHolders = 5;
inline constexpr std::size_t kStorageMinReshareHolders = 3;
inline constexpr std::size_t kStorageMaxHolders = 50;

// The storage secret rotates when the block time of a proposed record is at
// least this far past the block time that created the current key (B5a).
// Block time, never a local clock, decides it.
inline constexpr uint64_t kStorageRotationIntervalMs = 24ULL * 3600 * 1000;

// After a rotation the previous key (the retiring plan) still unwraps data keys
// while a server's latest finalized wrap claim names its S (B5a). This hard cap,
// in block time after the rotation's own block time, ends that period even when
// a server never returns. Then the holders destroy their old shares and refuse
// partials for it. While the retiring plan is live, no new rotation is selected,
// so at most one retiring plan exists.
inline constexpr uint64_t kStorageRetiringCapMs = 7 * kStorageRotationIntervalMs;

// Wrap claims (B5a, B7). A claim names a server's committed wrapped data key
// and, while it stages a rewrap or a replacement, the staged one. A batch
// record finalizes at most kStorageMaxClaimsPerBatch claims; the registry
// keeps one latest claim per server for at most kStorageMaxWrapOwners servers.
inline constexpr std::size_t kStorageMaxWrapsPerClaim = 2;
inline constexpr std::size_t kStorageMaxClaimsPerBatch = 64;
inline constexpr std::size_t kStorageMaxWrapOwners = 1024;

// A key generation or refresh session that has no finalized result this long
// (in block time) after its selection may be replaced: a retry without the
// holders whose round-1 silence this replica observed, or the next boundary's
// selection. Liveness only; a timeout never finalizes anything.
inline constexpr uint64_t kStorageSessionTimeoutMs = 120'000;
inline constexpr uint32_t kStorageMaxSessionAttempts = 3;

// A storage record's block time must lie within this bound of the voter's
// clock, as for a tree batch (design A4).
inline constexpr uint64_t kStorageBlockClockSkewMs = 60'000;

// Re-send cadence of a holder's own session packages and confirmation (the
// design Part C pattern). Receivers treat duplicates as benign.
inline constexpr uint64_t kStorageResendMs = 5000;

// Unlock budgets at each holder (B7): one pending request per requester, a
// per-requester limit per epoch, and a cap on all pending requests. A
// pending request expires after kStorageUnlockPendingMs.
inline constexpr uint32_t kStorageUnlocksPerRequesterPerEpoch = 8;
inline constexpr std::size_t kStorageMaxPendingUnlocks = 64;
inline constexpr uint64_t kStorageUnlockPendingMs = 30'000;
// Request digests a holder remembers per epoch to refuse replays.
inline constexpr std::size_t kStorageMaxRememberedRequests = 4096;

// Bound on one storage session message payload: a crate protocol message or a
// holder plan, plus the pairwise seal.
inline constexpr std::size_t kMaxStorageMessageBytes = 16 * 1024 + 128;
// Bound on a serialized holder plan (the crate's own bound).
inline constexpr std::size_t kMaxStoragePlanBytes = 16 * 1024;

// --- Release adoption and upgrade turns (Design-Auto-Update Phases 2 and 5) --

// Upgrade-health challenges one witness may issue for one subject at one
// upgrade chain position. A failed answer is retried; a new chain position
// opens a new bucket.
inline constexpr uint32_t kMaxUpgradeHealthAttemptsPerPosition = 8;

// Bound on one upgrade message body (an approval bundle, a signed status or
// observation, or an upgrade record), checked before parsing.
inline constexpr std::size_t kMaxUpgradeMessageBytes = 40 * 1024;

// Release-approval offers accepted from one peer per window. An offer costs a
// parse and two signature checks; the window bounds that work per peer.
inline constexpr uint64_t kReleaseOfferWindowMs = 10'000;

// Every server reports its upgrade status to the Tier 1 members on this
// cadence. The status is also the liveness signal for the turn rule.
inline constexpr uint64_t kUpgradeStatusIntervalMs = 15'000;

// A server whose last signed status is older than this is down for the turn
// rule. A vote-time input: a Turn record finalizes only when a consensus
// quorum independently saw every other participant live.
inline constexpr uint64_t kUpgradeLivenessWindowMs = 60'000;

// A status report is fresh for this long after its signed timestamp, so a
// captured Ready or Failed report cannot be replayed later.
inline constexpr uint64_t kUpgradeStatusMaxAgeMs = 120'000;

// A witness re-challenges the subject of an upgrade turn on this cadence.
inline constexpr uint64_t kUpgradeWitnessIntervalMs = 10'000;

// Most participants one rollout orders. Bounds the Start record.
inline constexpr std::size_t kMaxUpgradeParticipants = 512;

// --- Compile-time checks against the architecture tables --------------------

static_assert(max_byzantine_faults(5) == 1 && consensus_quorum(5) == 4);
static_assert(max_byzantine_faults(6) == 1 && consensus_quorum(6) == 5);
static_assert(max_byzantine_faults(7) == 2 && consensus_quorum(7) == 5);
static_assert(max_byzantine_faults(10) == 3 && consensus_quorum(10) == 7);
static_assert(max_byzantine_faults(31) == 10 && consensus_quorum(31) == 21);

static_assert(authority_threshold(5) == 5);
static_assert(authority_threshold(6) == 5);
static_assert(authority_threshold(7) == 5);
static_assert(authority_threshold(8) == 6);
static_assert(authority_threshold(31) == 21);

static_assert(tier1_target_count(4) == 0);
static_assert(tier1_target_count(5) == 5);
static_assert(tier1_target_count(24) == 7);
static_assert(tier1_target_count(25) == 10);
static_assert(tier1_target_count(10000) == 31);

}  // namespace nexus::security::constants
