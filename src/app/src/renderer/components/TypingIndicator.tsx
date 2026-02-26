import React from 'react';

const TypingIndicator: React.FC<{ size?: 'normal' | 'small' }> = ({ size = 'normal' }) => (
  <div className={`typing-indicator${size === 'small' ? ' small' : ''}`}>
    <span></span>
    <span></span>
    <span></span>
  </div>
);

export default TypingIndicator;
