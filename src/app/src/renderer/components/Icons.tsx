import React from 'react';

export const SendIcon: React.FC = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
    <path
      d="M22 2L11 13M22 2L15 22L11 13M22 2L2 9L11 13"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      transform="translate(-1, 1)"
    />
  </svg>
);

export const StopIcon: React.FC = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
    <rect x="6" y="6" width="12" height="12" fill="currentColor" rx="2" />
  </svg>
);

export const ImageUploadIcon: React.FC = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
    <path
      d="M21 19V5C21 3.9 20.1 3 19 3H5C3.9 3 3 3.9 3 5V19C3 20.1 3.9 21 5 21H19C20.1 21 21 20.1 21 19ZM8.5 13.5L11 16.51L14.5 12L19 18H5L8.5 13.5Z"
      fill="currentColor"
    />
  </svg>
);

export const MicrophoneIcon: React.FC<{ active?: boolean }> = ({ active = false }) => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
    <path
      d="M12 1C10.34 1 9 2.34 9 4V12C9 13.66 10.34 15 12 15C13.66 15 15 13.66 15 12V4C15 2.34 13.66 1 12 1Z"
      fill={active ? '#ef4444' : 'none'}
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
    />
    <path
      d="M19 10V12C19 15.87 15.87 19 12 19C8.13 19 5 15.87 5 12V10M12 19V23M8 23H16"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
    />
  </svg>
);
