import React from 'react';

export interface ContextBarItem {
  id: string;
  label: string;
  tooltip?: string;
}

interface RightPanelContextBarProps {
  items: ContextBarItem[];
  activeId: string;
  onSelect: (id: string) => void;
  rightSlot?: React.ReactNode;
}

const RightPanelContextBar: React.FC<RightPanelContextBarProps> = ({ items, activeId, onSelect, rightSlot }) => {
  return (
    <div className="right-panel-context-bar">
      <div className="right-panel-context-items" role="tablist" aria-label="Panel context">
        {items.map((item) => (
          <button
            key={item.id}
            type="button"
            className={`right-panel-context-item ${activeId === item.id ? 'active' : ''}`}
            onClick={() => onSelect(item.id)}
            role="tab"
            aria-selected={activeId === item.id}
            title={item.tooltip || item.label}
            aria-label={item.tooltip || item.label}
          >
            {item.label}
          </button>
        ))}
      </div>
      {rightSlot && <div className="right-panel-context-right">{rightSlot}</div>}
    </div>
  );
};

export default RightPanelContextBar;
