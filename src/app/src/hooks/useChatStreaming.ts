import { useState, useEffect, useRef, useCallback } from 'react';
import api, { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';
import { LEMONADE_TOOLS, executeTool, ToolCall } from '../tools/lemonadeTools';

const MAX_TOOL_ROUNDS = 5;

/** Produce a short human-readable summary of a tool result */
function summarizeResult(toolName: string, data: Record<string, unknown>): string {
  switch (toolName) {
    case 'list_models': return `${data.count} models found`;
    case 'get_model_info': return `${(data as any).display_name || (data as any).name || (data as any).id || 'model'} — ${((data as any).recipes || []).length} recipe(s)`;
    case 'load_model': return 'Model loaded';
    case 'unload_model': return 'Model unloaded';
    case 'get_loaded_models': {
      const loaded = (data as any).loaded;
      return Array.isArray(loaded) ? `${loaded.length} model(s) loaded` : JSON.stringify(data).slice(0, 80);
    }
    case 'get_server_health': return `${data.status} — ${data.loaded_models} model(s)`;
    case 'pull_model': return `${data.status}`;
    case 'delete_model': return 'Model deleted';
    case 'get_system_info': return 'System info retrieved';
    case 'list_backends': return `${Object.keys(data).length} recipe(s)`;
    case 'install_backend': return `${data.status}`;
    case 'ask_question': return 'Presenting choices';
    default: return JSON.stringify(data).slice(0, 80);
  }
}

export interface ToolCallEntry {
  name: string;
  args: string;
  result: string;
  status: 'running' | 'done' | 'error';
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
  send: (convoId: string, model: string, messages: ChatMessage[], useTools?: boolean) => Promise<void>;
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

  const send = useCallback(async (convoId: string, model: string, messages: ChatMessage[], useTools = false) => {
    setActiveStreams(prev => ({ ...prev, [convoId]: { content: '', thinking: '', toolCalls: [] } }));
    setThinkingExpanded(false);

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    let fullMessages = [...messages];

    let toolRound = 0;
    const allToolCalls: ToolCallEntry[] = [];

    const runCompletion = async (): Promise<void> => {
      return new Promise<void>((resolve, reject) => {
        api.chatCompletion(model, fullMessages, {
          tools: useTools ? LEMONADE_TOOLS as unknown as Record<string, unknown>[] : undefined,
          onReasoning: (_token, fullReasoning) => {
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '' };
            buf[convoId].thinking = fullReasoning;
            if (!thinkingExpanded) setThinkingExpanded(true);
          },
          onToken: (_token, full) => {
            const buf = tokenBufferRef.current;
            if (!buf[convoId]) buf[convoId] = { content: '', thinking: '' };
            buf[convoId].content = full;
          },
          onStats: (stats) => {
            setLiveStats(prev => ({ ...prev, [convoId]: stats }));
          },
          onToolCalls: async (toolCalls) => {
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
              return { name: tc.function.name, args: argsStr, result: '', status: 'running' as const };
            });
            allToolCalls.push(...runningEntries);
            setActiveStreams(prev => ({
              ...prev,
              [convoId]: { ...(prev[convoId] || { content: '', thinking: '', toolCalls: [] }), toolStatus: `Calling ${toolNames}…`, toolCalls: [...allToolCalls] },
            }));

            // Append the assistant's tool_calls message
            fullMessages = [
              ...fullMessages,
              { role: 'assistant' as const, content: null, tool_calls: toolCalls },
            ];

            // Execute all tool calls in parallel
            const results = await Promise.all(
              toolCalls.map(tc => executeTool(tc as ToolCall))
            );

            // Update entries with results
            for (let j = 0; j < runningEntries.length; j++) {
              const r = results[j];
              const entry = runningEntries[j];
              try {
                const parsed = JSON.parse(r.content);
                entry.result = parsed.error ? `Error: ${parsed.error}` : summarizeResult(entry.name, parsed);
                entry.status = parsed.error ? 'error' : 'done';
              } catch {
                entry.result = r.content.slice(0, 200);
                entry.status = 'done';
              }
            }

            // Append tool results
            for (const result of results) {
              fullMessages = [
                ...fullMessages,
                { role: 'tool' as const, content: result.content, tool_call_id: result.tool_call_id },
              ];
            }

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
