# Backend Engine Admission Policy

Decides whether a third-party inference engine gets integrated as a lemonade backend, and at what tier.

This is about admitting the **upstream project**. Download/execute hardening is a separate doc and applies to every backend at every tier.

---

## 1. Tiers

| Tier | Meaning | Shipped how |
|---|---|---|
| **Experimental** | Vetted, not trusted for defaults | In-tree, off by default, marked experimental |
| **Supported** | We stand behind it | Default-installable, in CI, release-blocking |

Everything enters at Experimental or below. Nothing goes straight to Supported, including vendor backends.

---

## 2. Value proposition (gate zero)

Answered before sponsorship, engine review, or scoring. Cheapest question to answer, most common reason to decline.

Every backend costs us forever: CI time, per-release validation, conformance runs, triage, another download path to secure, one more option in front of a user who just wants to be told what to pick. Burden of proof is on the newcomer. "It works" is not a value proposition.

### 2.1 Coverage matrix

Describe the backend as the cells it serves:

- **Device** — CPU, dGPU by API (Vulkan / ROCm / CUDA / Metal), iGPU, NPU
- **Format / architecture** — GGUF quant families, ONNX, safetensors; dense, MoE, hybrid/SSM, vision, embedding
- **Capability** — streaming, grammars, speculative decoding, batching, LoRA, tool calling, long context

Compare against the union of all Supported backends.

### 2.2 Qualifying paths

Meet at least one.

| Path | Threshold |
|---|---|
| **Coverage** | Uniquely serves ≥ 1 device class or ≥ 1 architecture family no Supported backend serves |
| **Performance** | ≥ 25% on decode or prefill, or ≥ 20% on peak memory, at equal output quality, identical hardware, ≥ 3 models, reproduced by someone other than the proposer |
| **Capability** | ≥ 2 capabilities absent from every Supported backend on that device class |
| **Stability** | ≥ 50% fewer crashes, hangs, or corrupt-output failures than the incumbent over a 72-hour soak on the same models and hardware — or the incumbent has a known stability class it can't fix and this one doesn't have |
| **Activity** | Ships support for new model architectures ≥ 30 days ahead of every Supported backend, measured across the last 3 upstream releases |
| **License / distribution** | Resolves a documented redistribution or compliance constraint blocking an existing backend |
| **Novelty** | See §2.3 |

Performance comparisons hold hardware constant. Vary the device alongside the engine and you've measured nothing.

Stability qualifies even if the backend is slower. A backend that never wedges is worth more than one that's 15% faster and hangs on long context.

Activity here is not §4's "is upstream alive." It's "does upstream ship faster than what we already have, in a way users feel." The metric that matters is day-0 support when a new model drops.

### 2.3 Kernel Novelty (exception path)

For backends that are genuinely different in implementation, not in packaging. Slower is acceptable. Narrower is acceptable. Novel is the claim.

Qualifies on:

- A **named mechanism** — a new kernel formulation, memory layout, scheduler, quantization scheme with its own kernels, numeric strategy, or execution model. Name it and explain it. "Different architecture" is not a name.
- **Not reachable by patching an incumbent.** If it could land as a PR to llamacpp, it should.

Terms:

- Exempt from §2.4 overlap and from the performance thresholds
- **Experimental only.** Never promoted to Supported on this path — re-qualify on a normal path first
- Requires a named champion, not just a sponsor
- **Mandatory review at 12 months.** If the approach hasn't proven out, it demotes. That's an acceptable outcome, not a failure — this path exists to let us carry bets.

Cap: no more than **two** active kernel-advancement backends at a time.

### 2.4 Redundancy shifts the burden

If more than **70%** of a backend's cells are already served by Supported backends, the Coverage path is closed. Qualify on Performance, Capability, Stability, or Activity with measurements, or get declined.

### 2.5 Disqualified without further review

- A wrapper around an engine we already integrate
- A fork of an existing backend whose delta could be an upstream PR — send the patch upstream
- Different packaging, different bindings, or a different API shape over equivalent capability
- Performance claims below §2.2 thresholds. That's churn, not value.

### 2.6 Replacement

If a candidate beats an incumbent across substantially all of its range, propose **replacement**, not coexistence. Name the backend being retired, the migration path, and the deprecation schedule.

Two overlapping backends nobody will delete is the failure mode this section prevents.

### 2.7 Unproven claims

Credible but not yet demonstrable gets Experimental with a re-evaluation date, max **180 days**. Not measured and reproduced by then, it demotes under §8.

---

## 3. Hard gates

Pass/fail. No points, no exceptions, no vendor override, no kernel-advancement exemption.

1. **License** — OSI-approved, compatible with redistributing prebuilt binaries
2. **Security contact** — documented channel, evidence someone reads it
3. **Artifacts built by upstream's public CI from a tagged commit.** Binaries uploaded from a maintainer's laptop are disqualifying
4. **Published checksums** for every artifact we consume
5. **No independent network activity** — no auto-update, no phone home, no fetching models or runtimes on its own. If it can, we must be able to turn it off, and we do, this would allow for a network analysis with wireshark on the backend without too much noise.
6. **Runs unprivileged**
7. **Named lemonade sponsor** who owns triage and pre-release validation
8. **Passes conformance** against the reference backend before merge

---

## 4. Health score

**Experimental ≥ 8. Supported ≥ 16.** Max 26.

| Signal | Criteria | Pts |
|---|---|---|
| **Age** | ≥ 6 mo since first public *release* | 1 |
| | ≥ 1 yr | 2 |
| | ≥ 2 yr | 3 |
| **Release cadence** | ≥ 1 release in last 12 mo | 1 |
| | ≥ 2 releases in last 6 mo | 2 |
| **Active maintainers** (merge rights, merged in last 90 d) | 2 | 1 |
| | 3–5 | 2 |
| | 6+ | 3 |
| **Non-maintainer contributors** (distinct, merged PR, last 12 mo) | ≥ 5 | 1 |
| | ≥ 15 | 2 |
| | ≥ 40 | 3 |
| **Bus factor** (top contributor's commit share, last 6 mo) | < 95% | 1 |
| | < 80% | 2 |
| **Issue response** (median time to first maintainer reply, last 90 d) | < 30 days | 1 |
| | < 7 days | 2 |
| **External PR merged** in last 90 d | yes | 1 |
| **Security posture** | Policy published | 1 |
| | Handled a real vuln with advisory + fix | 2 |
| **Input-handling rigor** | CI runs tests | 1 |
| | CI runs sanitizers or fuzzing on model/input parsing | 3 |
| **Release signing** | Artifacts signed | 2 |
| **Downstream adoption** | Distro-packaged, or ≥ 3 independent downstream consumers | 2 |
| **Popularity** | ≥ 1,000 stars or ≥ 200 forks | 1 |

Popularity is capped at 1 on purpose. It's the most gameable and least predictive signal here. Fuzzed parsers are worth 3 because backends parse untrusted model files inside our process tree.

**Vendor / first-party override.** A vendor-maintained or first-party backend can substitute a written support commitment (named engineering owner, support horizon, security channel) for *Age*, *Non-maintainer contributors*, *Downstream adoption*, and *Popularity* — max 9 pts. Hard gates and gate zero still apply in full.

---

## 5. Reviewers

| Action | Approvals |
|---|---|
| Admit at **Experimental** | 2 maintainers, one from security/platform |
| Admit on **kernel advancement** | 2 maintainers, one from security/platform, plus the named champion |
| Promote to **Supported** | 3 maintainers: 1 security, 1 platform owner (Windows / Linux / NPU), 1 unaffiliated with the sponsor |
| **Replacement** proposal | 3 maintainers, including the retiring backend's sponsor or two others if unavailable |
| Change download allowlist or manifest source | 2 maintainers, security mandatory, any tier |
| Demote or remove | 1 maintainer + sponsor notification |

- No self-approval. The sponsor's own approval doesn't count toward the total.
- Vendor-authored backends need one approval from someone unaffiliated with that vendor.
- Supported promotion: 14-day comment period, no unresolved maintainer objections.
- Soak: 90 days at Experimental across ≥ 1 lemonade release before Supported.
- Hardware: Supported needs the device in CI, or a named maintainer who owns it and validates before releases. We don't support what nobody can test.

---

## 6. Upstream engine review

Don't review the engine. Review what we're exposed to.

- **Build pipeline** — can we tie the binary to a commit?
- **Network behavior** — every outbound call it can make, and how to kill each one
- **Privilege and filesystem** — what it writes, where, whether any path is attacker-influenced, whether it loads libraries from directories we don't control
- **Untrusted input** — model parsing is the attack surface. Bounds checking, fuzzing, past parser CVEs
- **Dependencies** — vendored deps, currency, anything unmaintained we'd inherit
- **Vulnerability history** — not whether they've had CVEs, but response time and whether fixes shipped in a release we could consume
- **Process model** — can we supervise, kill, and resource-limit it cleanly?

Findings go in the admission issue. Open high-severity findings block admission at any tier.

---

## 7. Plugin code review

Normal code review, plus:

- Capabilities validated at load time with actionable errors
- No silent fallback — no quiet CPU substitution, precision downgrade, or backend swap
- Honors `offline` and `no_fetch_executables` on every path it adds
- Adds no new control surface that influences which code executes
- Conformance: fixed prompts, greedy decode, logits within stated tolerance vs the reference backend
- Benchmarks via the shared harness only, sufficient to back the §2.2 claim
- Failure-path coverage: engine missing, engine crashes mid-generation, incompatible device

---

## 8. Sunset

Backends go by policy, not argument. **Automatic demotion of one tier**, removal after 180 days in Experimental without remediation with exceptions and must fail multiple and is up to the discretion of the maintainer group:

- No upstream release in **12 months**
- Sponsor gone, not replaced in **90 days**
- Conformance fails **2 consecutive releases**
- Unpatched high-severity vulnerability open upstream **90 days**
- Validation hardware no longer available to any maintainer
- Provisional claim (§2.7) not demonstrated by its date
- Kernel-advancement backend fails its 12-month review (§2.3)

**Absorbed claim.** If another backend picks up the coverage or capability that was this one's qualifying claim, the sponsor has 90 days to re-qualify under §2.2 on current evidence. Otherwise it demotes, or someone opens a replacement proposal. This is what stops the list ratcheting upward as the ecosystem converges.

Demotion is announced one release ahead and goes in the changelog. Removal needs only that criteria were met and notice elapsed. No vote.

---

## 9. Process

1. Admission issue leads with the **value proposition**: coverage matrix, qualifying path, evidence. Fails gate zero, it closes here — before anyone spends review time.
2. A maintainer sponsors it, or it closes as unsponsored.
3. Hard gates evidenced, health score posted with sources.
4. Sponsor does the upstream review and posts findings.
5. Plugin PR, reviewed under §7 with §5 approvals.
6. Merged at Experimental. Soak starts.
7. Promotion after soak with fresh scoring and a re-stated value proposition measured on shipped code. Neither is inherited.

Supported backends re-verify value proposition and score **annually**. Drifted below 16, or the claim no longer holds, goes to §8.

---

## 10. Reviewer checklist

Work top to bottom. Stop at the first block that fails.

**Value proposition (§2)**
- [ ] Coverage matrix filed and compared against all Supported backends
- [ ] Qualifying path named: Coverage / Performance / Capability / Stability / Activity / License / Kernel advancement
- [ ] Evidence attached; measurements reproduced by someone other than the proposer
- [ ] Performance claims taken on identical hardware
- [ ] Overlap under 70%, or qualified on a non-Coverage path
- [ ] Not a wrapper, fork, or repackage (§2.5)
- [ ] If it beats an incumbent across its range, filed as replacement instead (§2.6), At discretion of Maintainer group.
- [ ] Provisional claims have a re-evaluation date on record, ≤ 180 days

**Hard gates (§3)** — all eight, no exceptions
- [ ] License OSI-approved, covers redistributing prebuilt binaries
- [ ] Security contact documented, evidence someone reads it
- [ ] Artifacts built by upstream public CI from a tagged commit
- [ ] Checksums published for every artifact we consume
- [ ] No independent network activity, or we can turn it off
- [ ] Sponsor named
- [ ] Conformance passes against the reference backend

**Health score (§4)**
- [ ] Score posted with a source for every row
- [ ] ≥ 8 for Experimental, ≥ 16 for Supported
- [ ] Vendor override, if used, backed by a written commitment naming owner, horizon, security channel

**Upstream engine (§6)**
- [ ] Binary traceable to a commit - Maintainer Group Discretion
- [ ] Outbound calls enumerated, each one killable
- [ ] Filesystem and privilege behavior reviewed; loads no libraries from directories we don't control
- [ ] Model-parsing surface reviewed; fuzzing and parser CVE history checked
- [ ] Dependency surface reviewed for unmaintained inherits
- [ ] Supervisable, killable, resource-limitable
- [ ] No open high-severity findings

**Plugin code (§7)**
- [ ] No per-backend branching in core
- [ ] Capabilities declared and validated at load time, errors actionable
- [ ] No silent fallback anywhere
- [ ] `offline` and `no_fetch_executables` honored on every path it adds
- [ ] No new control surface influencing which code executes
- [ ] Conformance: fixed prompts, greedy decode, logits within stated tolerance
- [ ] Benchmarks through the shared harness, sufficient to back the §2.2 claim
- [ ] Failure paths tested: engine missing, crash mid-generation, incompatible device

**Sign-off (§5)**
- [ ] Required approvals present, sponsor's own approval not counted
- [ ] Vendor backend has one approval from someone unaffiliated with that vendor
- [ ] Supported only: 90-day soak across ≥ 1 release
- [ ] Supported only: 14-day comment period elapsed, no unresolved objections
- [ ] Supported only: device in CI, or named maintainer validating pre-release
