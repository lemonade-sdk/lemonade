#pragma once

// Field codecs for the authority chain, shared by SecurityCodec.cpp (inside
// security messages) and AuthorityChainCodec.cpp (standalone pages). Not part
// of the public include tree.

#include "CodecPrimitives.hpp"

#include <LemonadeNexus/Security/Consensus/CommitProof.hpp>
#include <LemonadeNexus/Security/Consensus/ConsensusTypes.hpp>
#include <LemonadeNexus/Security/Epoch/AuthorityChainPage.hpp>
#include <LemonadeNexus/Security/Epoch/NextEpochPlan.hpp>
#include <LemonadeNexus/Security/Genesis/BootstrapCertificate.hpp>

namespace nexus::security::codec_detail {

bool encode_certificate(Writer& w, const QuorumCertificate& qc);
/// A proposal header followed by the certificate it extends.
bool encode_proposal_core(Writer& w, const Proposal& p, const QuorumCertificate& justify);
bool encode_bootstrap(Writer& w, const BootstrapCertificate& c);
bool encode_committed_block(Writer& w, const CommittedBlock& b);
bool encode_commit_proof(Writer& w, const CommitProof& p);
bool encode_handoff(Writer& w, const EpochHandoff& h);
bool encode_handoff_proof(Writer& w, const EpochHandoffProofMsg& m);
bool encode_chain_request(Writer& w, const AuthorityChainRequest& c);
bool encode_chain_page(Writer& w, const AuthorityChainPage& p);

bool decode_certificate(Reader& r, QuorumCertificate& qc);
bool decode_proposal_core(Reader& r, Proposal& p, QuorumCertificate& justify);
bool decode_bootstrap(Reader& r, BootstrapCertificate& c);
bool decode_committed_block(Reader& r, CommittedBlock& b);
bool decode_commit_proof(Reader& r, CommitProof& p);
bool decode_handoff(Reader& r, EpochHandoff& h);
bool decode_handoff_proof(Reader& r, EpochHandoffProofMsg& m);
bool decode_chain_request(Reader& r, AuthorityChainRequest& c);
bool decode_chain_page(Reader& r, AuthorityChainPage& p);

}  // namespace nexus::security::codec_detail
