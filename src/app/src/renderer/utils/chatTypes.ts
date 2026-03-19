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
  type: 'audio';
  audio: {
    data: string;
    mime: string;
    name?: string;
  };
}

export type MessageContent = string | Array<TextContent | ImageContent | AudioContent>;

export interface Message {
  role: 'user' | 'assistant';
  content: MessageContent;
  thinking?: string;
}
