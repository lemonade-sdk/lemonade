import { useState, useEffect, useRef, useCallback } from 'react';
import type { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';
import type { ToolCall, ToolResult } from '../tools/lemonadeTools';
import { ToolLoopDetector } from './toolLoopDetector';

export type { ToolCall } from '../tools/lemonadeTools';

export type ChatThinkingMode = 'normal' | 'off';

export function chatThinkingParams(mode: ChatThinkingMode): Record<string, unknown> {
  return { enable_thinking: mode !== 'off' };
}


const TOOL_FOLLOWUP_PROMPT = [
  "Use the tool result to answer the user's last request with concrete, specific details.",
  'Do not stop at a raw count such as "105 models" or at "success". Present the actual names/status/details returned by the tool.',
  'If a tool result has status=needs_choice, call ask_question with the returned choices/candidates. Otherwise answer now in natural language.',
  'Do not call the same tool with the same arguments again unless the result explicitly asks you to.',
].join(' ');

export interface ToolArtifact {
  type: 'image' | 'audio' | 'model3d';
  url: string;
  name?: string;
  mime?: string;
}

export interface ToolExecutionPayload extends ToolResult {
  displayResult?: string;
  artifacts?: ToolArtifact[];
  error?: boolean;
}

export interface ChatToolRuntime {
  tools: Record<string, unknown>[];
  execute: (call: ToolCall) => Promise<ToolExecutionPayload>;
  systemPrompt?: string;
}

/** Produce a short human-readable summary of a tool result */
function summarizeResult(toolName: string, data: Record<string, unknown>): string {
  switch (toolName) {
    case 'list_models': {
      const counts = (data as any).counts || {};
      const total = Number(counts.loaded || 0) + Number(counts.downloaded || 0);
      if (Number.isFinite(total) && total > 0) return `${total} local model(s): ${counts.loaded || 0} loaded, ${counts.downloaded || 0} downloaded`;
      if (counts.registry) return `${counts.registry} registry model(s)`;
      return 'Model inventory retrieved';
    }
    case 'get_model_info': return `${(data as any).display_name || (data as any).name || (data as any).id || 'model'}: ${((data as any).recipes || []).length} recipe(s)`;
    case 'load_model': return (data as any).mode === 'router' ? 'Router ready' : 'Model loaded';
    case 'unload_model': return (data as any).mode === 'router' ? 'Router has no runtime to unload' : 'Model unloaded';
    case 'get_loaded_models': {
      const loaded = (data as any).loaded;
      return Array.isArray(loaded) ? `${loaded.length} model(s) loaded` : JSON.stringify(data).slice(0, 80);
    }
    case 'get_server_health': return `${data.status}: ${data.loaded_models} model(s)`;
    case 'pull_model': {
      const anyData = data as any;
      if (anyData.status === 'needs_choice') {
        const items = anyData.choices || anyData.candidates || anyData.variants || [];
        return Array.isArray(items) ? `Needs choice: ${items.slice(0, 4).map((item: any) => item.id || item.name || item).join(', ')}` : 'Needs choice';
      }
      return `${anyData.status || 'download complete'}${anyData.model ? `: ${anyData.model}` : ''}`;
    }
    case 'delete_model': return 'Model deleted';
    case 'get_system_info': {
      const anyData = data as any;
      const counts = anyData.counts || {};
      const devices = anyData.devices || {};
      const deviceNames = Object.entries(devices).flatMap(([kind, items]: [string, any]) => Array.isArray(items) ? items.map((item: any) => `${kind}: ${item.name || 'unknown'}`) : []);
      return [`System info: ${counts.recipes || 0} recipe(s), ${counts.installed_backends || 0} installed backend(s)`, ...deviceNames.slice(0, 4)].join(' · ');
    }
    case 'list_backends': {
      const recipes = (data as any).recipes || data;
      return `${Object.keys(recipes || {}).length} recipe(s): ${Object.keys(recipes || {}).slice(0, 5).join(', ')}`;
    }
    case 'install_backend': return `${data.status}`;
    case 'ask_question': return 'Presenting choices';
    default: return JSON.stringify(data).slice(0, 80);
  }
}

export interface ToolCallEntry {
  name: string;
  args: string;
  rawArgs?: string;
  result: string;
  status: 'running' | 'done' | 'error';
  artifacts?: ToolArtifact[];
}


interface StreamState {
  content: string;
  thinking: string;
  toolStatus?: string;
  toolCalls: ToolCallEntry[];
}

export interface ChatStreamingResult {
  activeStreams: Record<string, StreamState>;
  liveStats: Record<string, LiveStreamStats>;
  thinkingExpanded: boolean;
  setThinkingExpanded: React.Dispatch<React.SetStateAction<boolean>>;
  streamingConvoIds: Set<string>;
  getStream: (convoId: string) => StreamState | undefined;
  getLiveStats: (convoId: string) => LiveStreamStats | undefined;
  send: (
    convoId: string,
    model: string,
    messages: ChatMessage[],
    tools?: boolean | ChatToolRuntime | null,
    thinkingMode?: ChatThinkingMode,
  ) => Promise<void>;
  stop: (convoId: string) => { content: string; thinking?: string } | null;
}

export function useChatStreaming(
  onDone: (convoId: string, stats: ChatCompletionStats, toolCalls?: ToolCallEntry[]) => void,
  onError: (convoId: string, message: string) => void,
): ChatStreamingResult {
  const [activeStreams, setActiveStreams] = useState<Record<string, StreamState>>({});
  const [liveStats, setLiveStats] = useState<Record<string, LiveStreamStats>>({});
  const [thinkingExpanded, setThinkingExpanded] = useState(false);

  const controllersRef = useRef<Map<string, AbortController>>(new Map());
  const tokenBufferRef = useRef<Record<string, StreamState>>({});
  const flushTimerRef = useRef<number | null>(null);

  // Do not keep a 50ms interval alive for the entire lifetime of the Chat view.
  // Schedule a flush only when a streaming token actually arrives.
  const flushTokenBuffer = useCallback(() => {
    flushTimerRef.current = null;
    const buf = tokenBufferRef.current;
    const keys = Object.keys(buf);
    if (keys.length === 0) return;
    setActiveStreams(prev => {
      let next = prev;
      for (const id of keys) {
        if (!prev[id]) continue;
        if (next === prev) next = { ...prev };
        next[id] = { ...next[id], ...buf[id] };
      }
      return next;
    });
    tokenBufferRef.current = {};
  }, []);

  const scheduleTokenFlush = useCallback(() => {
    if (flushTimerRef.current !== null) return;
    flushTimerRef.current = window.setTimeout(flushTokenBuffer, 50);
  }, [flushTokenBuffer]);

  useEffect(() => () => {
    if (flushTimerRef.current !== null) window.clearTimeout(flushTimerRef.current);
  }, []);

  const getStream = useCallback(
    (convoId: string) => activeStreams[convoId],
    [activeStreams],
  );

  const getLiveStats = useCallback(
    (convoId: string) => liveStats[convoId],
    [liveStats],
  );

  const send = useCallback(async (
    convoId: string,
    model: string,
    messages: ChatMessage[],
    tools: boolean | ChatToolRuntime | null = false,
    thinkingMode: ChatThinkingMode = 'normal',
  ) => {
    let runtime: ChatToolRuntime | null = tools && typeof tools === 'object' ? tools : null;
    if (tools === true) {
      const { LEMONADE_TOOLS, executeTool } = await import(
        /* webpackChunkName: "lemonade-tools" */ '../tools/lemonadeTools'
      );
      runtime = {
        tools: LEMONADE_TOOLS as unknown as Record<string, unknown>[],
        execute: executeTool as unknown as (call: ToolCall) => Promise<ToolExecutionPayload>,
      };
    }
    setActiveStreams(prev => ({ ...prev, [convoId]: { content: '', thinking: '', toolCalls: [] } }));
    setThinkingExpanded(false);

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    const ownsConversation = () => controllersRef.current.get(convoId) === controller;
    const cleanupIfOwned = () => { if (ownsConversation()) cleanup(convoId); };
    const dropTokenBufferIfOwned = () => { if (ownsConversation()) delete tokenBufferRef.current[convoId]; };

    let fullMessages = [...messages];
    // When a small context is exhausted by tool schemas, retry with exactly
    // the original conversation and attachments, omitting the tool-only prompt.
    const withoutToolPrompt = runtime?.systemPrompt
      && messages[0]?.role === 'system'
      && messages[0].content === runtime.systemPrompt
      ? messages.slice(1) : messages;
    let retriedWithoutTools = false;
    const hasImageInput = messages.some(message => Array.isArray(message.content)
      && message.content.some(part => part.type === 'image_url'));

    const toolLoopDetector = new ToolLoopDetector();
    let toolsDisabled = false;
    let blockedOnlyRounds = 0;
    const allToolCalls: ToolCallEntry[] = [];

    const runCompletion = async (): Promise<void> => {
      const { default: api } = await import(/* webpackChunkName: "api-client" */ '../api');
      return new Promise<void>((resolve, reject) => {
        const withoutToolSystemPrompt = toolsDisabled && runtime?.systemPrompt
          && fullMessages[0]?.role === 'system' && fullMessages[0].content === runtime.systemPrompt
          ? fullMessages.slice(1) : fullMessages;
        api.chatCompletion(model, retriedWithoutTools ? withoutToolPrompt : withoutToolSystemPrompt, {
          params: chatThinkingParams(thinkingMode),
          tools: !toolsDisabled && !retriedWithoutTools && runtime?.tools?.length ? runtime.tools : undefined,
          onReasoning: (_token, fullReasoning) => {
            if (!ownsConversation()) return;
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '', toolCalls: [] };
            buf[convoId].thinking = fullReasoning;
            scheduleTokenFlush();
            if (!thinkingExpanded) setThinkingExpanded(true);
          },
          onToken: (_token, full) => {
            if (!ownsConversation()) return;
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '', toolCalls: [] };
            buf[convoId].content = full;
            scheduleTokenFlush();
          },
          onStats: (stats) => {
            if (!ownsConversation()) return;
            setLiveStats(prev => ({ ...prev, [convoId]: stats }));
          },
          onToolCalls: async (toolCalls) => {
            try {
              if (!ownsConversation() || controller.signal.aborted) {
                dropTokenBufferIfOwned();
                resolve();
                return;
              }
              if (toolsDisabled) {
                onError(convoId, 'The model requested tools despite loop protection disabling tool calls.');
                cleanupIfOwned();
                resolve();
                return;
              }

              // Show tool status in the stream and add running entries
              const toolNames = toolCalls.map(tc => tc.function.name).join(', ');
              const runningEntries: ToolCallEntry[] = toolCalls.map(tc => {
                let argsStr = '';
                try { const a = JSON.parse(tc.function.arguments || '{}'); argsStr = Object.entries(a).map(([k,v]) => `${k}: ${v}`).join(', '); } catch {}
                return { name: tc.function.name, args: argsStr, rawArgs: tc.function.arguments, result: '', status: 'running' as const };
              });
              allToolCalls.push(...runningEntries);
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: `Calling ${toolNames}…`, toolCalls: [...allToolCalls] },
              }));

              // Append the assistant's tool_calls message
              fullMessages = [
                ...fullMessages,
                { role: 'assistant' as const, content: '', tool_calls: toolCalls },
              ];

              // Skip only invocations already blocked by repeated identical errors.
              const blockedReasons = toolCalls.map(tc => toolLoopDetector.blockedReason(tc));
              const results: ToolExecutionPayload[] = await Promise.all(toolCalls.map((tc, i) => {
                const reason = blockedReasons[i];
                return reason ? {
                  role: 'tool' as const,
                  tool_call_id: tc.id,
                  content: JSON.stringify({ error: `Skipped: ${reason}` }),
                  displayResult: `Skipped: ${reason}`,
                  error: true,
                } : runtime!.execute(tc as ToolCall);
              }));

              // Tool execution takes no abort signal, so a stop mid-call lands
              // here with a dead controller.
              if (controller.signal.aborted) {
                dropTokenBufferIfOwned();
                resolve();
                return;
              }

              // Update entries with results
              for (let j = 0; j < runningEntries.length; j++) {
                const r = results[j];
                const entry = runningEntries[j];
                entry.artifacts = r.artifacts;
                if (r.displayResult) {
                  entry.result = r.displayResult;
                  entry.status = r.error ? 'error' : 'done';
                  continue;
                }
                try {
                  const parsed = JSON.parse(r.content);
                  entry.result = parsed.error ? `Error: ${parsed.error}` : summarizeResult(entry.name, parsed);
                  entry.status = parsed.error ? 'error' : 'done';
                } catch {
                  entry.result = r.content.slice(0, 200);
                  entry.status = r.error ? 'error' : 'done';
                }
              }

              // Keep every tool result in the model's history, including unsuccessful ones.
              for (const result of results) {
                fullMessages = [
                  ...fullMessages,
                  { role: 'tool' as const, content: result.content, tool_call_id: result.tool_call_id },
                ];
              }
              const executedCalls = toolCalls.filter((_, i) => !blockedReasons[i]);
              const executedResults = results.filter((_, i) => !blockedReasons[i]);
              const onlyBlocked = toolCalls.length > 0 && executedCalls.length === 0;
              blockedOnlyRounds = onlyBlocked ? blockedOnlyRounds + 1 : 0;
              const loopNotice = blockedOnlyRounds >= 2
                ? { action: 'stop' as const, reason: 'The model continued requesting only blocked tool calls' }
                : toolLoopDetector.observe(executedCalls, executedResults);
              if (loopNotice) {
                console.warn(`[Chat tools] ${loopNotice.action}: ${loopNotice.reason}`);
                if (loopNotice.action === 'stop') {
                  toolsDisabled = true;
                  allToolCalls.push({
                    name: 'loop_protection',
                    args: '(automatic)',
                    result: `${loopNotice.reason}. Further tool calls have been disabled for this response.`,
                    status: 'error',
                  });
                }
              }
              const guidance = loopNotice?.action === 'stop'
                ? `${loopNotice.reason}. Tools are now disabled for this response. Explain what you completed and what remains unfinished, without claiming further actions succeeded. Include any relevant generated code in your answer for the user to inspect.`
                : loopNotice?.action === 'warn'
                  ? `${TOOL_FOLLOWUP_PROMPT}

Possible tool loop: ${loopNotice.reason}. Do not repeat identical calls with unchanged results. Try a different approach, wait for real progress, or explain the blocker.`
                  : blockedReasons.some(Boolean)
                    ? `${TOOL_FOLLOWUP_PROMPT}

Some tool calls were skipped after repeated identical failures. Other tools remain available; change the blocked call's arguments or choose a different tool.`
                    : TOOL_FOLLOWUP_PROMPT;
              fullMessages = [
                ...fullMessages,
                { role: 'user' as const, content: guidance },
              ];

              // Clear tool status, keep accumulated tool call entries
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }),
                  toolStatus: loopNotice?.action === 'stop' ? 'Tool loop detected; finishing without tools...' : undefined,
                  toolCalls: [...allToolCalls] },
              }));

              // Re-run completion with tool results
              try {
                await runCompletion();
                resolve();
              } catch (err) {
                reject(err);
              }
            } catch (err: unknown) {
              if (controller.signal.aborted) {
                dropTokenBufferIfOwned();
                cleanupIfOwned();
                resolve();
                return;
              }
              // Mark any still-running tool call entries as errored so the user sees what happened.
              const msg = err instanceof Error ? err.message : String(err);
              for (const entry of allToolCalls) {
                if (entry.status === 'running') {
                  entry.result = `Error: ${msg}`;
                  entry.status = 'error';
                }
              }
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: undefined, toolCalls: [...allToolCalls] },
              }));
              dropTokenBufferIfOwned();
              onError(convoId, `Tool execution failed: ${msg}`);
              cleanupIfOwned();
              resolve();
            }
          },
          onDone: (stats) => {
            if (!ownsConversation()) {
              resolve();
              return;
            }

            dropTokenBufferIfOwned();

            if (controller.signal.aborted) {
              cleanupIfOwned();
              resolve();
              return;
            }

            if (
              !retriedWithoutTools &&
              hasImageInput &&
              runtime?.tools?.length &&
              allToolCalls.length === 0 &&
              (!stats.content.trim() || stats.finishReason === 'length')
            ) {
              retriedWithoutTools = true;

              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { content: '', thinking: '', toolCalls: [] },
              }));
              setLiveStats(prev => {
                const next = { ...prev };
                delete next[convoId];
                return next;
              });
              setThinkingExpanded(false);

              void runCompletion().then(resolve, reject);
              return;
            }

            if (stats.finishReason === 'length') {
              onError(convoId, 'The model reached its context or output token limit (finish_reason=length). Increase context size or reduce chat history/tools.');
            } else if (!stats.content.trim() && allToolCalls.length === 0) {
              onError(convoId, 'The model returned no visible answer (reasoning-only or empty completion). Its context may be full; increase context size or disable tools.');
            } else {
              onDone(convoId, stats, allToolCalls.length > 0 ? [...allToolCalls] : undefined);
            }

            cleanupIfOwned();
            resolve();
          },
          onError: (err) => {
            if (err.name === 'AbortError') { cleanupIfOwned(); resolve(); return; }
            dropTokenBufferIfOwned();
            onError(convoId, err.message);
            cleanupIfOwned();
            resolve();
          },
          signal: controller.signal,
        });
      });
    };

    const cleanup = (id: string) => {
      setActiveStreams(prev => {
        const next = { ...prev };
        delete next[id];
        return next;
      });
      setLiveStats(prev => {
        const next = { ...prev };
        delete next[id];
        return next;
      });
      controllersRef.current.delete(id);
    };

    await runCompletion();
  }, [onDone, onError, scheduleTokenFlush]);

  const stop = useCallback((convoId: string): { content: string; thinking?: string } | null => {
    const controller = controllersRef.current.get(convoId);
    if (!controller) return null;

    const buf = tokenBufferRef.current[convoId];
    const stream = activeStreams[convoId];
    const merged = stream && buf
      ? { content: buf.content || stream.content, thinking: buf.thinking || stream.thinking }
      : stream;
    delete tokenBufferRef.current[convoId];

    controller.abort();
    controllersRef.current.delete(convoId);

    setActiveStreams(prev => {
      const next = { ...prev };
      delete next[convoId];
      return next;
    });
    setLiveStats(prev => {
      const next = { ...prev };
      delete next[convoId];
      return next;
    });

    if (merged && (merged.content || merged.thinking)) {
      return { content: merged.content || '(stopped)', thinking: merged.thinking || undefined };
    }
    return null;
  }, [activeStreams]);

  return {
    activeStreams,
    liveStats,
    thinkingExpanded,
    setThinkingExpanded,
    streamingConvoIds: new Set(Object.keys(activeStreams)),
    getStream,
    getLiveStats,
    send,
    stop,
  };
}
