import type { ToolCallEntry } from '../../hooks/useChatStreaming';

// Tool outputs remain available in memory while the conversation is open.
// Local chat history stores only text metadata, never embedded media payloads.
const DATA_URL = /data:[^;,\s"'<>]+;base64,[A-Za-z0-9+/=]+/gi;
const LARGE_BASE64 = /[A-Za-z0-9+/]{1024,}={0,2}/g;

function redactEmbeddedMedia(value: string): string {
  return value.replace(DATA_URL, '[media omitted]').replace(LARGE_BASE64, '[media omitted]');
}

export function prepareToolCallsForStorage(calls?: ToolCallEntry[]): ToolCallEntry[] | undefined {
  return calls?.map(({ artifacts: _artifacts, ...call }) => ({
    ...call,
    args: redactEmbeddedMedia(call.args),
    rawArgs: call.rawArgs === undefined ? undefined : redactEmbeddedMedia(call.rawArgs),
    result: redactEmbeddedMedia(call.result),
  }));
}
