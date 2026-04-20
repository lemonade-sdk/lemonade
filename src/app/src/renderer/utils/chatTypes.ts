export interface ImageContent {
  type: 'image_url';
  image_url: {
    url: string;
  };
}

export interface TextContent {
  type: 'text';
  text: string;
}

export interface AudioContent {
  type: 'input_audio';
  input_audio: {
    data: string;
    format?: string;
  };
}

export type MessageContent = string | Array<TextContent | ImageContent | AudioContent>;

export interface Message {
  role: 'user' | 'assistant';
  content: MessageContent;
  thinking?: string;
}

// Wire protocol types for tool-calling agentic loop
export interface ToolCallFunction {
  name: string;
  arguments: string;
}

export interface ToolCall {
  id: string;
  type: 'function';
  function: ToolCallFunction;
}

export interface ToolMessage {
  role: 'tool';
  tool_call_id: string;
  content: string;
}

export interface AssistantMessage {
  role: 'assistant';
  content: string | null;
  tool_calls?: ToolCall[];
}

export interface Artifact {
  type: 'image' | 'audio';
  data: string;
  mime: string;
}

export interface UploadedAudio {
  dataUrl: string;
  base64: string;
  format: string;
  filename: string;
}
