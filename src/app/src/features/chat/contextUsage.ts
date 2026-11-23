import type { ChatMessage } from '../../api';

/** A single inference request; cached input tokens are already included in promptTokens. */
export interface ChatContextUsage {
  promptTokens: number;
  completionTokens: number | null;
  cachedTokens: number | null;
  systemTokens: number;
  toolDefinitionTokens: number;
  conversationTokens: number;
  toolResultTokens: number;
  otherTokens: number;
  /** The model tokenizer is not available before the first completed request. */
  promptEstimated?: boolean;
  /** Live output or missing backend usage: output is estimated from text length. */
  completionEstimated?: boolean;
  /** Estimated split of generated tokens. Thinking is not sent again as chat history. */
  thinkingTokens?: number;
  answerTokens?: number;
}

export function estimatedTextTokensFromLength(length: number): number {
  return length > 0 ? Math.ceil(length / 4) : 0;
}

function estimatedTextTokens(text: string): number {
  return estimatedTextTokensFromLength(text.length);
}

function messageText(message: ChatMessage): string {
  if (typeof message.content === 'string') return message.content;
  if (!Array.isArray(message.content)) return '';
  // Image/audio payload bytes are not text tokens. The measured prompt's
  // remaining cost is assigned to Other, not counted as base64 text.
  return message.content
    .filter(part => part.type === 'text')
    .map(part => part.type === 'text' ? part.text : '')
    .join('\n');
}

function tokenCount(value: unknown): number | null {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 ? value : null;
}

/**
 * Estimate only the known prompt contents. No network request is required,
 * so the system prompt and enabled tool schemas can show before the first turn.
 */
export function estimateChatContextUsage(
  messages: ChatMessage[],
  tools: Record<string, unknown>[] | undefined,
): ChatContextUsage {
  let systemTokens = 0;
  let conversationTokens = 0;
  let toolResultTokens = 0;
  for (const message of messages) {
    const estimate = estimatedTextTokens(messageText(message));
    if (message.role === 'system') systemTokens += estimate;
    else if (message.role === 'tool') toolResultTokens += estimate;
    else conversationTokens += estimate;
  }
  let toolDefinitionTokens = 0;
  if (tools?.length) {
    try { toolDefinitionTokens = estimatedTextTokens(JSON.stringify(tools)); }
    catch { /* Bad tool schema must not break a completion. */ }
  }
  return {
    promptTokens: systemTokens + toolDefinitionTokens + conversationTokens + toolResultTokens,
    completionTokens: null,
    cachedTokens: null,
    systemTokens,
    toolDefinitionTokens,
    conversationTokens,
    toolResultTokens,
    otherTokens: 0,
    promptEstimated: true,
    completionEstimated: true,
    thinkingTokens: 0,
    answerTokens: 0,
  };
}

/**
 * Reconcile the estimated categories with a real backend prompt count.
 * Templates, media, and uncounted metadata are assigned to Other; a rough
 * text estimate that exceeds the backend count is scaled down (truncation).
 */
export function buildChatContextUsage(
  messages: ChatMessage[],
  tools: Record<string, unknown>[] | undefined,
  promptTokens: number | null,
  completionTokens: number | null,
  cachedTokens: number | null,
): ChatContextUsage | null {
  const measuredPrompt = tokenCount(promptTokens);
  if (measuredPrompt === null) return null;
  const rough = estimateChatContextUsage(messages, tools);
  const subtotal = rough.promptTokens;
  const scale = subtotal > measuredPrompt ? measuredPrompt / subtotal : 1;
  const systemTokens = Math.floor(rough.systemTokens * scale);
  const toolDefinitionTokens = Math.floor(rough.toolDefinitionTokens * scale);
  const conversationTokens = Math.floor(rough.conversationTokens * scale);
  const toolResultTokens = Math.floor(rough.toolResultTokens * scale);
  const otherTokens = measuredPrompt - systemTokens - toolDefinitionTokens - conversationTokens - toolResultTokens;
  const reportedCompletion = tokenCount(completionTokens);
  return {
    promptTokens: measuredPrompt,
    completionTokens: reportedCompletion,
    cachedTokens: tokenCount(cachedTokens),
    systemTokens,
    toolDefinitionTokens,
    conversationTokens,
    toolResultTokens,
    otherTokens,
    promptEstimated: false,
    completionEstimated: reportedCompletion === null,
    thinkingTokens: 0,
    answerTokens: reportedCompletion ?? 0,
  };
}

/**
 * Add generation progress without confusing SSE chunks with actual tokens.
 * The total uses backend completion_tokens once available; otherwise text
 * length estimates are explicitly marked approximate. Reasoning is part of
 * completion_tokens, not an extra amount to add to the measured total.
 */
export function withGeneratedContextUsage(
  input: ChatContextUsage,
  answerLength: number,
  thinkingLength: number,
  reportedCompletion: number | null,
): ChatContextUsage {
  const reported = tokenCount(reportedCompletion);
  const estimatedThinking = estimatedTextTokensFromLength(thinkingLength);
  const estimatedAnswer = estimatedTextTokensFromLength(answerLength);
  const estimatedTotal = estimatedThinking + estimatedAnswer;
  const completionTokens = reported ?? estimatedTotal;
  const thinkingTokens = estimatedTotal > 0
    ? Math.min(completionTokens, Math.round(completionTokens * estimatedThinking / estimatedTotal))
    : 0;
  return {
    ...input,
    completionTokens,
    completionEstimated: reported === null,
    thinkingTokens,
    answerTokens: completionTokens - thinkingTokens,
  };
}

/** Usage-only SSE chunks and llama.cpp timings can arrive in either order. */
export interface ChatTokenCounts {
  usagePrompt: number | null;
  usageCompletion: number | null;
  usageCache: number | null;
  timingPrompt: number | null;
  timingCompletion: number | null;
  timingCache: number | null;
}

export function newChatTokenCounts(): ChatTokenCounts {
  return {
    usagePrompt: null, usageCompletion: null, usageCache: null,
    timingPrompt: null, timingCompletion: null, timingCache: null,
  };
}

function object(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown> : null;
}

/** Mutates only six tiny counters; no tokenized strings are retained. */
export function collectChatTokenCounts(counts: ChatTokenCounts, rawChunk: unknown): void {
  const chunk = object(rawChunk);
  if (!chunk) return;
  const timings = object(chunk.timings);
  if (timings) {
    const processed = tokenCount(timings.prompt_n);
    const cached = tokenCount(timings.cache_n);
    // llama.cpp prompt_n excludes cached tokens; usage.prompt_tokens does not.
    if (processed !== null) counts.timingPrompt = processed + (cached ?? 0);
    counts.timingCompletion = tokenCount(timings.predicted_n) ?? counts.timingCompletion;
    counts.timingCache = cached ?? counts.timingCache;
  }
  const usage = object(chunk.usage);
  if (usage) {
    counts.usagePrompt = tokenCount(usage.prompt_tokens ?? usage.input_tokens) ?? counts.usagePrompt;
    counts.usageCompletion = tokenCount(usage.completion_tokens ?? usage.output_tokens) ?? counts.usageCompletion;
    const details = object(usage.prompt_tokens_details) || object(usage.input_tokens_details);
    if (details) counts.usageCache = tokenCount(details.cached_tokens) ?? counts.usageCache;
  }
}
