export const LEMONADE_MCP_SERVER_ID = 'lemonade';
export const MAX_MCP_SERVER_SELECTION = 4;
// This is the implicit/default count. Opt-in web_search is deliberately excluded.
export const LEMONADE_MCP_TOOL_COUNT = 19;
export const WEB_SEARCH_TOOL_NAME = 'web_search';
export const isDefaultMcpTool = (name: string): boolean => name !== WEB_SEARCH_TOOL_NAME;

export interface McpServerOption {
  id: string;
  name: string;
  transport: 'builtin' | 'streamable-http' | 'stdio';
  connected: boolean;
  status: string;
  tools: number;
  lastError?: string;
}

export interface McpToolOption {
  name: string;
  runtimeName: string;
  title?: string;
  description?: string;
}

export interface McpServerToolOption extends McpServerOption {
  toolOptions: McpToolOption[];
}
