# Nightingale Architecture

## Routing Layer

Nightingale's router (`nrouter`) sits between client SDKs and the
accelerator pool. Every request carries a `latency_budget_ms` field;
`nrouter` picks the fastest accelerator whose current queue depth can
satisfy that budget, using a decaying-average queue estimator with a
half-life of 4 seconds.

## The Talon Cache

The Talon Cache is Nightingale's KV-cache reuse layer. It was
introduced in build `nightingale-0.4.0` to cut repeated-prompt latency.
Talon keys cache entries by a SHA-256 hash of the tokenized prompt
prefix, and evicts entries using a cost-aware LRU that weighs both
recency and the original compute cost to regenerate that prefix.

Talon's default cache budget is 512 MB per accelerator, configurable
via the `NIGHTINGALE_TALON_BUDGET_MB` environment variable.

## Failover Path

If an accelerator misses three consecutive heartbeats (heartbeat
interval: 2 seconds, so ~6 seconds total), `nrouter` marks it
`suspect` and stops sending new requests to it, but does not kill
in-flight requests. After 30 seconds with no heartbeat, the node is
marked `dead` and its in-flight requests are retried on the next-best
accelerator.
