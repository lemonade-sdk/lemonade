export const LEMONADE_MCP_SERVER_ID = 'lemonade';
export const MAX_MCP_SERVER_SELECTION = 4;
export const LEMONADE_MCP_TOOL_COUNT = 6;

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
