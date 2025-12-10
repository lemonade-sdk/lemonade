import React, { useRef, useEffect, useState } from 'react';

export type PanelOrientation = 'horizontal' | 'vertical';
export type PanelSide = 'left' | 'right' | 'top' | 'bottom';

interface ResizablePanelProps {
  children: React.ReactNode;
  orientation?: PanelOrientation;
  defaultSize?: number;
  minSize?: number;
  maxSize?: number;
  showDivider?: boolean;
  showDividerBefore?: boolean; // Show divider before content (for resizing from left)
  className?: string;
  fillSpace?: boolean; // If true, use flex: 1 instead of fixed size
}

const ResizablePanel: React.FC<ResizablePanelProps> = ({
  children,
  orientation = 'vertical',
  defaultSize,
  minSize = orientation === 'vertical' ? 200 : 100,
  maxSize = orientation === 'vertical' ? 800 : 600,
  showDivider = false,
  showDividerBefore = false,
  className = '',
  fillSpace = false,
}) => {
  const [size, setSize] = useState<number | undefined>(defaultSize);
  const isDraggingRef = useRef(false);
  const startPosRef = useRef(0);
  const startSizeRef = useRef(0);
  const panelRef = useRef<HTMLDivElement>(null);

  const isVertical = orientation === 'vertical';

  useEffect(() => {
    const handleMouseMove = (e: MouseEvent) => {
      if (!isDraggingRef.current) return;

      let delta = isVertical
        ? e.clientX - startPosRef.current
        : e.clientY - startPosRef.current;

      // For dividers before content, reverse the delta (drag left to grow)
      if ((isDraggingRef as any).isBefore) {
        delta = -delta;
      }

      const newSize = Math.max(minSize, Math.min(maxSize, startSizeRef.current + delta));
      setSize(newSize);
    };

    const handleMouseUp = () => {
      if (isDraggingRef.current) {
        isDraggingRef.current = false;
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
      }
    };

    document.addEventListener('mousemove', handleMouseMove);
    document.addEventListener('mouseup', handleMouseUp);

    return () => {
      document.removeEventListener('mousemove', handleMouseMove);
      document.removeEventListener('mouseup', handleMouseUp);
    };
  }, [isVertical, minSize, maxSize]);

  const handleDividerMouseDown = (e: React.MouseEvent, isBefore: boolean) => {
    e.preventDefault();
    isDraggingRef.current = true;
    startPosRef.current = isVertical ? e.clientX : e.clientY;
    startSizeRef.current = size || (panelRef.current ? (isVertical ? panelRef.current.offsetWidth : panelRef.current.offsetHeight) : defaultSize || minSize);

    // Store whether this is a before divider for use in mouse move
    (isDraggingRef as any).isBefore = isBefore;

    document.body.style.cursor = isVertical ? 'col-resize' : 'row-resize';
    document.body.style.userSelect = 'none';
  };

  const style: React.CSSProperties = fillSpace
    ? { flex: 1, minWidth: 0, minHeight: 0 }
    : {
        flexShrink: 0,
        ...(size ? (isVertical ? { width: `${size}px` } : { height: `${size}px` }) : {}),
      };

  return (
    <>
      {showDividerBefore && (
        <div
          className={`resizable-divider ${isVertical ? '' : 'horizontal'}`}
          onMouseDown={(e) => handleDividerMouseDown(e, true)}
        />
      )}
      <div ref={panelRef} className={`resizable-panel-container ${className}`} style={style}>
        {children}
      </div>
      {showDivider && (
        <div
          className={`resizable-divider ${isVertical ? '' : 'horizontal'}`}
          onMouseDown={(e) => handleDividerMouseDown(e, false)}
        />
      )}
    </>
  );
};

export default ResizablePanel;
