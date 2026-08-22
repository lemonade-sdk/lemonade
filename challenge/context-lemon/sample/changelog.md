# Changelog

## nightingale-0.6.2 — 2025-09-18
- Fixed a race in the Talon Cache eviction thread that could double-free
  a cache slot under high concurrency (reported as INCIDENT-2241).

## nightingale-0.6.0 — 2025-08-01
- Added the Sparrow-CPU fallback path for devices with no accelerator.
- Raised the default `latency_budget_ms` from 800 to 1200.

## nightingale-0.5.0 — 2025-05-22
- Public beta opened to the first 50 design-partner accounts.

## nightingale-0.4.0 — 2025-02-10
- Introduced the Talon Cache (see `architecture.md`).
- Default Talon cache budget set to 512 MB per accelerator.

## nightingale-0.2.0 — 2024-07-01
- First multi-accelerator release; added Kestrel-GPU support alongside
  the original Falcon-NPU-only routing.

## nightingale-0.1.0-alpha — 2024-03-14
- Initial internal release. Falcon-NPU routing only, no caching layer.
