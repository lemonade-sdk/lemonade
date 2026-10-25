#include <LemonadeNexus/Security/Transport/AuthorityChainCodec.hpp>

#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>

#include "AuthorityChainCodecDetail.hpp"

namespace nexus::security {

namespace codec_detail {

bool encode_certificate(Writer& w, const QuorumCertificate& qc) {
    if (qc.signers.size() > constants::kMaxQcSignatures) {
        return false;
    }
    w.u16(qc.qc_format_version);
    w.u16(qc.consensus_ruleset);
    w.fixed(qc.network_id);
    w.u64(qc.epoch);
    w.u64(qc.height);
    w.u64(qc.view);
    w.fixed(qc.proposal_digest);
    w.u16(static_cast<uint16_t>(qc.signers.size()));
    for (const auto& signer : qc.signers) {
        put_node(w, signer.node_id);
        w.fixed(signer.signature);
    }
    return true;
}

bool encode_proposal_core(Writer& w, const Proposal& p, const QuorumCertificate& justify) {
    w.u16(p.security_ruleset);
    w.u16(p.consensus_ruleset);
    w.fixed(p.network_id);
    w.u64(p.epoch);
    w.u64(p.height);
    w.u64(p.view);
    put_node(w, p.leader);
    w.fixed(p.parent_digest);
    w.fixed(p.justify_qc_digest);
    w.fixed(p.previous_state_root);
    w.fixed(p.proposed_state_root);
    w.fixed(p.transitions_digest);
    w.u64(p.timestamp_hint);
    return encode_certificate(w, justify);
}

bool encode_bootstrap(Writer& w, const BootstrapCertificate& c) {
    w.fixed(c.network_id);
    w.u64(c.epoch);
    w.fixed(c.tier1_set_digest);
    w.u64(c.authority_threshold);
    w.fixed(c.authority_public_key);
    w.fixed(c.dkg_transcript_digest);
    w.fixed(c.attestation_root);
    w.fixed(c.founding_eligibility_digest);
    w.fixed(c.vote_key_set_digest);
    w.fixed(c.release_approval_digest);
    w.u16(c.security_ruleset);
    w.u16(c.consensus_ruleset);
    w.fixed(c.genesis_signature);
    return true;
}

bool encode_committed_block(Writer& w, const CommittedBlock& b) {
    return encode_proposal_core(w, b.proposal, b.justify);
}

bool encode_commit_proof(Writer& w, const CommitProof& p) {
    if (p.chain.size() != 3) {
        return false;
    }
    for (const auto& block : p.chain) {
        if (!encode_committed_block(w, block)) {
            return false;
        }
    }
    return encode_certificate(w, p.certifying);
}

bool encode_handoff(Writer& w, const EpochHandoff& h) {
    if (h.members.size() > constants::kMaxActiveTier1) {
        return false;
    }
    w.fixed(h.network_id);
    w.u64(h.from_epoch);
    w.u64(h.to_epoch);
    w.fixed(h.plan_digest);
    w.fixed(h.previous_anchor);
    w.u16(static_cast<uint16_t>(h.members.size()));
    for (const auto& node : h.members) {
        put_node(w, node);
        const auto incarnation = h.incarnations.find(node);
        w.u64(incarnation != h.incarnations.end() ? incarnation->second : 0);
        const auto key = h.vote_keys.find(node);
        if (key == h.vote_keys.end()) {
            return false;
        }
        w.fixed(key->second);
    }
    w.fixed(h.group_public_key);
    w.fixed(h.dkg_transcript_digest);
    w.u64(h.key_generation);
    w.fixed(h.attestation_root);
    w.u16(h.security_ruleset);
    w.u16(h.consensus_ruleset);
    return true;
}

bool encode_handoff_proof(Writer& w, const EpochHandoffProofMsg& m) {
    return encode_handoff(w, m.handoff) && encode_commit_proof(w, m.proof);
}

bool encode_chain_request(Writer& w, const AuthorityChainRequest& c) {
    w.u64(c.have_epoch);
    return true;
}

bool encode_chain_page(Writer& w, const AuthorityChainPage& p) {
    if (p.base_vote_keys.size() > constants::kMaxActiveTier1 ||
        p.links.size() > constants::kMaxHandoffChainLinks) {
        return false;
    }
    w.u16(p.has_base ? 1 : 0);
    if (p.has_base) {
        encode_bootstrap(w, p.base_certificate);
        w.u16(static_cast<uint16_t>(p.base_vote_keys.size()));
        for (const auto& [node, key] : p.base_vote_keys) {
            put_node(w, node);
            w.fixed(key);
        }
    }
    w.u16(static_cast<uint16_t>(p.links.size()));
    for (const auto& link : p.links) {
        if (!encode_handoff_proof(w, link)) {
            return false;
        }
    }
    return true;
}

bool decode_certificate(Reader& r, QuorumCertificate& qc) {
    uint16_t count = 0;
    if (!(r.u16(qc.qc_format_version) && r.u16(qc.consensus_ruleset) && r.fixed(qc.network_id) &&
          r.u64(qc.epoch) && r.u64(qc.height) && r.u64(qc.view) && r.fixed(qc.proposal_digest) &&
          r.u16(count))) {
        return false;
    }
    // Bound before the signer loop: a claimed count never drives allocation
    // or signature work past the compiled maximum.
    if (count > constants::kMaxQcSignatures) {
        return r.fail(CodecError::CountTooLarge);
    }
    qc.signers.resize(count);
    for (auto& signer : qc.signers) {
        if (!(get_node(r, signer.node_id) && r.fixed(signer.signature))) {
            return false;
        }
    }
    return true;
}

bool decode_proposal_core(Reader& r, Proposal& p, QuorumCertificate& justify) {
    return r.u16(p.security_ruleset) && r.u16(p.consensus_ruleset) && r.fixed(p.network_id) &&
           r.u64(p.epoch) && r.u64(p.height) && r.u64(p.view) && get_node(r, p.leader) &&
           r.fixed(p.parent_digest) && r.fixed(p.justify_qc_digest) &&
           r.fixed(p.previous_state_root) && r.fixed(p.proposed_state_root) &&
           r.fixed(p.transitions_digest) && r.u64(p.timestamp_hint) &&
           decode_certificate(r, justify);
}

bool decode_bootstrap(Reader& r, BootstrapCertificate& c) {
    uint64_t threshold = 0;
    if (!(r.fixed(c.network_id) && r.u64(c.epoch) && r.fixed(c.tier1_set_digest) &&
          r.u64(threshold) && r.fixed(c.authority_public_key) && r.fixed(c.dkg_transcript_digest) &&
          r.fixed(c.attestation_root) && r.fixed(c.founding_eligibility_digest) &&
          r.fixed(c.vote_key_set_digest) && r.fixed(c.release_approval_digest) &&
          r.u16(c.security_ruleset) && r.u16(c.consensus_ruleset) &&
          r.fixed(c.genesis_signature))) {
        return false;
    }
    c.authority_threshold = static_cast<std::size_t>(threshold);
    return true;
}

bool decode_committed_block(Reader& r, CommittedBlock& b) {
    return decode_proposal_core(r, b.proposal, b.justify);
}

bool decode_commit_proof(Reader& r, CommitProof& p) {
    p.chain.resize(3);
    for (auto& block : p.chain) {
        if (!decode_committed_block(r, block)) {
            return false;
        }
    }
    return decode_certificate(r, p.certifying);
}

bool decode_handoff(Reader& r, EpochHandoff& h) {
    uint16_t count = 0;
    if (!(r.fixed(h.network_id) && r.u64(h.from_epoch) && r.u64(h.to_epoch) &&
          r.fixed(h.plan_digest) && r.fixed(h.previous_anchor) && r.u16(count))) {
        return false;
    }
    if (count > constants::kMaxActiveTier1) {
        return r.fail(CodecError::CountTooLarge);
    }
    for (uint16_t i = 0; i < count; ++i) {
        NodeId node;
        uint64_t incarnation = 0;
        nexus::crypto::Ed25519PublicKey key{};
        if (!(get_node(r, node) && r.u64(incarnation) && r.fixed(key))) {
            return false;
        }
        h.members.push_back(node);
        h.incarnations[node] = incarnation;
        h.vote_keys[node] = key;
    }
    return r.fixed(h.group_public_key) && r.fixed(h.dkg_transcript_digest) &&
           r.u64(h.key_generation) && r.fixed(h.attestation_root) &&
           r.u16(h.security_ruleset) && r.u16(h.consensus_ruleset);
}

bool decode_handoff_proof(Reader& r, EpochHandoffProofMsg& m) {
    return decode_handoff(r, m.handoff) && decode_commit_proof(r, m.proof);
}

bool decode_chain_request(Reader& r, AuthorityChainRequest& c) { return r.u64(c.have_epoch); }

bool decode_chain_page(Reader& r, AuthorityChainPage& p) {
    uint16_t has_base = 0;
    if (!r.u16(has_base) || has_base > 1) {
        return r.fail(CodecError::BadValue);
    }
    p.has_base = has_base == 1;
    if (p.has_base) {
        if (!decode_bootstrap(r, p.base_certificate)) {
            return false;
        }
        uint16_t base_count = 0;
        if (!r.u16(base_count)) {
            return false;
        }
        if (base_count > constants::kMaxActiveTier1) {
            return r.fail(CodecError::CountTooLarge);
        }
        for (uint16_t i = 0; i < base_count; ++i) {
            NodeId node;
            nexus::crypto::Ed25519PublicKey key{};
            if (!(get_node(r, node) && r.fixed(key))) {
                return false;
            }
            p.base_vote_keys.emplace_back(node, key);
        }
    }
    uint16_t count = 0;
    if (!r.u16(count)) {
        return false;
    }
    // Bounded before the loop: a claimed count never drives allocation or
    // signature parsing past the compiled page limit.
    if (count > constants::kMaxHandoffChainLinks) {
        return r.fail(CodecError::CountTooLarge);
    }
    for (uint16_t i = 0; i < count; ++i) {
        EpochHandoffProofMsg link;
        if (!decode_handoff_proof(r, link)) {
            return false;
        }
        p.links.push_back(std::move(link));
    }
    return true;
}

}  // namespace codec_detail

std::vector<uint8_t> encode_authority_chain_page(const AuthorityChainPage& page) {
    codec_detail::Writer w;
    if (!codec_detail::encode_chain_page(w, page)) {
        return {};
    }
    return w.take();
}

std::optional<AuthorityChainPage> decode_authority_chain_page(std::span<const uint8_t> bytes) {
    if (bytes.size() > constants::kMaxSecurityMessageBytes) {
        return std::nullopt;
    }
    codec_detail::Reader r(bytes);
    AuthorityChainPage page;
    if (!codec_detail::decode_chain_page(r, page) || !r.done()) {
        return std::nullopt;
    }
    return page;
}

}  // namespace nexus::security
