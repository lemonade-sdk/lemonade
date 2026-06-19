import { useState, useEffect, useRef, useCallback } from 'react';
import api, { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';
import { LEMONADE_TOOLS, executeTool, ToolCall, ToolResult } from '../tools/lemonadeTools';

export type { ToolCall } from '../tools/lemonadeTools';

const MAX_TOOL_ROUNDS = 5;
const TOOL_FOLLOWUP_PROMPT = [
  "Use the tool result to answer the user's last request with concrete, specific details.",
  'Do not stop at a raw count such as "105 models" or at "success". Present the actual names/status/details returned by the tool.',
  'If a tool result has status=needs_choice, call ask_question with the returned choices/candidates. Otherwise answer now in natural language.',
  'Do not call the same tool with the same arguments again unless the result explicitly asks you to.',
].join(' ');

export interface ToolArtifact {
  type: 'image' | 'audio';
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

const DEFAULT_TOOL_RUNTIME: ChatToolRuntime = {
  tools: LEMONADE_TOOLS as unknown as Record<string, unknown>[],
  execute: executeTool as unknown as (call: ToolCall) => Promise<ToolExecutionPayload>,
};

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
    case 'get_model_info': return `${(data as any).display_name || (data as any).name || (data as any).id || 'model'} — ${((data as any).recipes || []).length} recipe(s)`;
    case 'load_model': return 'Model loaded';
    case 'unload_model': return 'Model unloaded';
    case 'get_loaded_models': {
      const loaded = (data as any).loaded;
      return Array.isArray(loaded) ? `${loaded.length} model(s) loaded` : JSON.stringify(data).slice(0, 80);
    }
    case 'get_server_health': return `${data.status} — ${data.loaded_models} model(s)`;
    case 'pull_model': {
      const anyData = data as any;
      if (anyData.status === 'needs_choice') {
        const items = anyData.choices || anyData.candidates || anyData.variants || [];
        return Array.isArray(items) ? `Needs choice: ${items.slice(0, 4).map((item: any) => item.id || item.name || item).join(', ')}` : 'Needs choice';
      }
      return `${anyData.status || 'download complete'}${anyData.model ? ` — ${anyData.model}` : ''}`;
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
  send: (convoId: string, model: string, messages: ChatMessage[], tools?: boolean | ChatToolRuntime | null) => Promise<void>;
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

  // Flush token buffer → state (50ms interval)
  useEffect(() => {
    const flush = setInterval(() => {
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
    }, 50);
    return () => clearInterval(flush);
  }, []);

  const getStream = useCallback(
    (convoId: string) => activeStreams[convoId],
    [activeStreams],
  );

  const getLiveStats = useCallback(
    (convoId: string) => liveStats[convoId],
    [liveStats],
  );

  const send = useCallback(async (convoId: string, model: string, messages: ChatMessage[], tools: boolean | ChatToolRuntime | null = false) => {
    const runtime: ChatToolRuntime | null = tools === true
      ? DEFAULT_TOOL_RUNTIME
      : (tools && typeof tools === 'object' ? tools : null);
    setActiveStreams(prev => ({ ...prev, [convoId]: { content: '', thinking: '', toolCalls: [] } }));
    setThinkingExpanded(false);

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    let fullMessages = runtime?.systemPrompt
      ? [{ role: 'system' as const, content: runtime.systemPrompt }, ...messages]
      : [...messages];

    let toolRound = 0;
    const allToolCalls: ToolCallEntry[] = [];

    const runCompletion = async (): Promise<void> => {
      return new Promise<void>((resolve, reject) => {
        api.chatCompletion(model, fullMessages, {
          tools: runtime?.tools?.length ? runtime.tools : undefined,
          onReasoning: (_token, fullReasoning) => {
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '', toolCalls: [] };
            buf[convoId].thinking = fullReasoning;
            if (!thinkingExpanded) setThinkingExpanded(true);
          },
          onToken: (_token, full) => {
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '', toolCalls: [] };
            buf[convoId].content = full;
          },
          onStats: (stats) => {
            setLiveStats(prev => ({ ...prev, [convoId]: stats }));
          },
          onToolCalls: async (toolCalls) => {
            try {
              toolRound++;
              if (toolRound > MAX_TOOL_ROUNDS) {
                onError(convoId, 'Too many tool call rounds — stopping to prevent loops.');
                cleanup(convoId);
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

              // Execute all tool calls in parallel
              const results = await Promise.all(
                toolCalls.map(tc => runtime!.execute(tc as ToolCall))
              );

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

              // Append tool results, then remind the model to produce a real final
              // answer (or call a narrower tool) instead of echoing a broad count.
              for (const result of results) {
                fullMessages = [
                  ...fullMessages,
                  { role: 'tool' as const, content: result.content, tool_call_id: result.tool_call_id },
                ];
              }
              fullMessages = [
                ...fullMessages,
                { role: 'user' as const, content: TOOL_FOLLOWUP_PROMPT },
              ];

              // Clear tool status, keep accumulated tool call entries
              setActiveStreams(prev => ({
                ...prev,
                [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: undefined, toolCalls: [...allToolCalls] },
              }));

              // Re-run completion with tool results
              try {
                await runCompletion();
                resolve();
              } catch (err) {
                reject(err);
              }
            } catch (err: unknown) {
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
              delete tokenBufferRef.current[convoId];
              onError(convoId, `Tool execution failed: ${msg}`);
              cleanup(convoId);
              resolve();
            }
          },
          onDone: (stats) => {
            delete tokenBufferRef.current[convoId];
            onDone(convoId, stats, allToolCalls.length > 0 ? [...allToolCalls] : undefined);
            cleanup(convoId);
            resolve();
          },
          onError: (err) => {
            if (err.name === 'AbortError') { resolve(); return; }
            delete tokenBufferRef.current[convoId];
            onError(convoId, err.message);
            cleanup(convoId);
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
  }, [onDone, onError]);

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
