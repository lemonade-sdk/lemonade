import React from 'react';
import { ChatErrorInfo } from '../utils/httpErrors';

interface ChatErrorMessageProps {
  error: ChatErrorInfo;
}

const ChatErrorMessage: React.FC<ChatErrorMessageProps> = ({ error }) => (
  <div className="chat-error" role="alert">
    <div className="chat-error-title">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
        <circle cx="12" cy="12" r="10" />
        <line x1="12" y1="8" x2="12" y2="12" />
        <line x1="12" y1="16" x2="12.01" y2="16" />
      </svg>
      <span>{error.title}</span>
    </div>
    <p className="chat-error-detail">{error.detail}</p>
    {error.command && (
      <pre className="chat-error-command"><code>{error.command}</code></pre>
    )}
  </div>
);

export default ChatErrorMessage;
