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
  /** Image attachments in the request; image tokenization is backend-specific. */
  imageCount?: number;
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

/**
 * llama.cpp divides configured ctx_size between parallel slots unless the KV
 * cache is unified. Use the model's launch arguments: /slots belongs to the
 * most recently used model, which may not be this conversation's model.
 */
export function effectiveSlotContextSize(
  configured: number | null,
  recipe: string | null | undefined,
  launchCommand: string[] | null | undefined,
): number | null {
  if (configured === null || !Number.isFinite(configured) || configured <= 0) return null;
  if (!recipe?.toLowerCase().startsWith('llamacpp') || !launchCommand?.length) return configured;

  let parallel = 1;
  let unified = false;
  for (let i = 0; i < launchCommand.length; i++) {
    const arg = launchCommand[i];
    const eq = arg.indexOf('=');
    const flag = eq < 0 ? arg : arg.slice(0, eq);
    const value = eq < 0 ? launchCommand[i + 1] : arg.slice(eq + 1);
    if (flag === '--parallel' || flag === '-np') {
      const n = Number(value);
      if (Number.isInteger(n)) parallel = n;
    } else if (flag === '--kv-unified' || flag === '-kvu') {
      unified = true;
    } else if (flag === '--no-kv-unified' || flag === '-no-kvu') {
      unified = false;
    }
  }
  return parallel > 1 && !unified ? Math.max(1, Math.floor(configured / parallel)) : configured;
}

function estimatedTextTokens(text: string): number {
  return estimatedTextTokensFromLength(text.length);
}

// Placeholder until the backend reports real multimodal prompt usage.
// Never count base64 bytes as tokens: image encoders use model-specific patches.
export const ESTIMATED_IMAGE_TOKENS = 1024;

function imageCountInMessage(message: ChatMessage): number {
  return Array.isArray(message.content)
    ? message.content.filter(part => part.type === 'image_url').length : 0;
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
  let imageCount = 0;
  for (const message of messages) {
    imageCount += imageCountInMessage(message);
    const toolCallText = message.role === 'assistant'
      ? (message.tool_calls || []).map(call => `${call.function.name} ${call.function.arguments}`).join('\n')
      : '';
    const estimate = estimatedTextTokens(messageText(message)) + estimatedTextTokens(toolCallText);
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
    promptTokens: systemTokens + toolDefinitionTokens + conversationTokens + toolResultTokens + imageCount * ESTIMATED_IMAGE_TOKENS,
    completionTokens: null,
    cachedTokens: null,
    systemTokens,
    toolDefinitionTokens,
    conversationTokens,
    toolResultTokens,
    otherTokens: imageCount * ESTIMATED_IMAGE_TOKENS,
    imageCount,
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
  // A zero-token timing can mean fully cached/unsupported accounting, not an
  // empty prompt. Do not erase real system/tool categories in that case.
  if (measuredPrompt === 0 && rough.promptTokens > 0) return null;
  // Only scale the known text slices. The image placeholder is not a measured
  // cost and must never shrink the system prompt or tool definitions.
  const textSubtotal = rough.systemTokens + rough.toolDefinitionTokens
    + rough.conversationTokens + rough.toolResultTokens;
  const scale = textSubtotal > measuredPrompt ? measuredPrompt / textSubtotal : 1;
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
    imageCount: rough.imageCount,
    promptEstimated: false,
    completionEstimated: reportedCompletion === null,
    thinkingTokens: 0,
    answerTokens: reportedCompletion ?? 0,
  };
}

/**
 * Keep a previously observed image footprint while estimating the next request.
 * It is still an estimate, but avoids dropping a large multimodal contribution
 * to zero between completed requests and during the next stream.
 */
export function carryMeasuredImageCost(
  current: ChatContextUsage,
  previous: ChatContextUsage | null | undefined,
): ChatContextUsage {
  if (!current.promptEstimated || !current.imageCount || !previous?.imageCount
      || previous.promptEstimated || previous.otherTokens <= 0) return current;
  const observedPerImage = Math.max(
    ESTIMATED_IMAGE_TOKENS, Math.round(previous.otherTokens / previous.imageCount),
  );
  const estimatedOther = Math.max(current.otherTokens, current.imageCount * observedPerImage);
  return {
    ...current,
    promptTokens: current.promptTokens + estimatedOther - current.otherTokens,
    otherTokens: estimatedOther,
  };
}

/** Preview newly attached images before sending them to the model. */
export function withPendingImageCost(
  current: ChatContextUsage,
  pendingCount: number,
  previous: ChatContextUsage | null | undefined,
): ChatContextUsage {
  if (!Number.isSafeInteger(pendingCount) || pendingCount <= 0) return current;
  const observedPerImage = previous?.imageCount && !previous.promptEstimated
    ? Math.max(ESTIMATED_IMAGE_TOKENS, Math.round(previous.otherTokens / previous.imageCount))
    : ESTIMATED_IMAGE_TOKENS;
  const added = pendingCount * observedPerImage;
  return {
    ...current,
    promptTokens: current.promptTokens + added,
    otherTokens: current.otherTokens + added,
    imageCount: (current.imageCount ?? 0) + pendingCount,
    promptEstimated: true,
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
