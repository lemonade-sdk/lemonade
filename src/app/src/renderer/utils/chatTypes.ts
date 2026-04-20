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

export interface UploadedAudio {
  dataUrl: string;
  base64: string;
  format: string;
  filename: string;
}
