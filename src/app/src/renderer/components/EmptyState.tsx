import React from 'react';
// @ts-ignore - SVG assets live outside of the TypeScript rootDir for Electron packaging
import logoSvg from '../../../assets/logo.svg';

const EmptyState: React.FC<{ title: string }> = ({ title }) => (
  <div className="chat-empty-state">
    <img src={logoSvg} alt="Lemonade Logo" className="chat-empty-logo" />
    <h2 className="chat-empty-title">{title}</h2>
  </div>
);

export default EmptyState;
