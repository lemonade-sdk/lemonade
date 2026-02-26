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

export type MessageContent = string | Array<TextContent | ImageContent>;

export interface Message {
  role: 'user' | 'assistant';
  content: MessageContent;
  thinking?: string;
}
