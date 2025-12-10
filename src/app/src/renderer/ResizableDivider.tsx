import React from 'react';

interface ResizableDividerProps {
  onMouseDown: (e: React.MouseEvent) => void;
  orientation?: 'vertical' | 'horizontal';
}

const ResizableDivider: React.FC<ResizableDividerProps> = ({ onMouseDown, orientation = 'vertical' }) => {
  return (
    <div
      className={`resizable-divider ${orientation === 'horizontal' ? 'horizontal' : ''}`}
      onMouseDown={onMouseDown}
    />
  );
};

export default ResizableDivider;
