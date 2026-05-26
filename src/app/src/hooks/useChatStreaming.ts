import { useState, useEffect, useRef, useCallback } from 'react';
import api, { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';

interface StreamState {
  content: string;
  thinking: string;
}

export interface ChatStreamingResult {
  activeStreams: Record<string, StreamState>;
  liveStats: Record<string, LiveStreamStats>;
  thinkingExpanded: boolean;
  setThinkingExpanded: React.Dispatch<React.SetStateAction<boolean>>;
  streamingConvoIds: Set<string>;
  getStream: (convoId: string) => StreamState | undefined;
  getLiveStats: (convoId: string) => LiveStreamStats | undefined;
  send: (convoId: string, model: string, messages: ChatMessage[]) => Promise<void>;
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

  const send = useCallback(async (convoId: string, model: string, messages: ChatMessage[]) => {
    setActiveStreams(prev => ({ ...prev, [convoId]: { content: '', thinking: '' } }));
    setThinkingExpanded(false);

    const controller = new AbortController();
    controllersRef.current.set(convoId, controller);

    await api.chatCompletion(model, messages, {
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
      onDone: (stats) => {
        delete tokenBufferRef.current[convoId];
        onDone(convoId, stats);
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
        controllersRef.current.delete(convoId);
      },
      onError: (err) => {
        if (err.name === 'AbortError') return;
        delete tokenBufferRef.current[convoId];
        onError(convoId, err.message);
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
        controllersRef.current.delete(convoId);
      },
      signal: controller.signal,
    });
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
