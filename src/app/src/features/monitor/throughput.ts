import type { SlotData, StatsData } from '../../api';

/** llama.cpp has shipped flat, object, and array-shaped next_token fields. */
export function decodedTokensInSlot(slot: SlotData): number {
  const next = Array.isArray(slot.next_token) ? slot.next_token[0] : slot.next_token;
  const decoded = slot.n_decoded ?? next?.n_decoded;
  return typeof decoded === 'number' && Number.isFinite(decoded)
    ? Math.max(0, decoded)
    : 0;
}

/** Respect explicit slot activity; old windowed deltas must not keep an idle slot alive. */
export function isSlotProcessing(slot: SlotData, measuredTps = 0): boolean {
  if (typeof slot.is_processing === 'boolean') return slot.is_processing;
  if (slot.state === 'processing') return true;
  if (slot.state === 'idle') return false;
  return measuredTps > 0.05;
}

/** Display the actual two-second sample, not an artificial 200ms ramp. */
export function sampledSlotTps(target: number, active: boolean): number {
  return active && Number.isFinite(target) && target >= 0.05 ? target : 0;
}

function positiveNumber(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) && value > 0 ? value : 0;
}

/**
 * Prefer exact slot counter deltas. They may be unavailable when a request
 * finishes between 2-second polls, so use server live telemetry in that case.
 * A completed request's authoritative TPS is sampled once; old /stats data
 * must not turn into a permanent, fake live reading.
 */
export function selectGenerationTps(
  slotTps: number,
  stats: StatsData | null,
  newCompletion: boolean,
  supportsSlots: boolean,
): number {
  const fromLive = positiveNumber(stats?.live?.estimated_tokens_per_second);
  if (!supportsSlots) {
    // Backends like FastFlowLM have no slots. Preserve the live server metric.
    if (fromLive > 0) return fromLive;
  } else {
    const fromSlots = positiveNumber(slotTps);
    if (fromSlots > 0.05) return fromSlots;
    if ((stats?.live?.active_requests ?? 0) > 0 && fromLive > 0) return fromLive;
  }

  return newCompletion && (stats?.output_tokens ?? 0) > 0
    ? positiveNumber(stats?.tokens_per_second)
    : 0;
}
