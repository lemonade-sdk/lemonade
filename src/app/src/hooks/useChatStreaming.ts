import { useState, useEffect, useRef, useCallback } from 'react';
import api, { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';
import { LEMONADE_TOOLS, TOOLS_SYSTEM_PROMPT, executeTool, ToolCall } from '../tools/lemonadeTools';

const MAX_TOOL_ROUNDS = 5;

interface StreamState {
  content: string;
  thinking: string;
  toolStatus?: string; // Shows "Calling load_model…" etc.
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
  onDone: (convoId: string, stats: ChatCompletionStats) => void,
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
    setActiveStreams(prev => ({ ...prev, [convoId]: { content: '', thinking: '' } }));
    setThinkingExpanded(false);

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    // Prepend tools system prompt if tools are enabled
    let fullMessages = useTools
      ? [{ role: 'system' as const, content: TOOLS_SYSTEM_PROMPT }, ...messages]
      : messages;

    let toolRound = 0;

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

            // Show tool status in the stream
            const toolNames = toolCalls.map(tc => tc.function.name).join(', ');
            setActiveStreams(prev => ({
              ...prev,
              [convoId]: { ...(prev[convoId] || { content: '', thinking: '' }), toolStatus: `Calling ${toolNames}…` },
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

            // Append tool results
            for (const result of results) {
              fullMessages = [
                ...fullMessages,
                { role: 'tool' as const, content: result.content, tool_call_id: result.tool_call_id },
              ];
            }

            // Clear tool status
            setActiveStreams(prev => ({
              ...prev,
              [convoId]: { ...(prev[convoId] || { content: '', thinking: '' }), toolStatus: undefined },
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
            onDone(convoId, stats);
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
