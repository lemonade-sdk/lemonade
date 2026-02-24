import React from 'react';
import RightPanelContextBar, { ContextBarItem } from './RightPanelContextBar';

interface RightPanelTitleAreaProps {
  title?: string;
  titleNode?: React.ReactNode;
  contextItems?: ContextBarItem[];
  activeContextId?: string;
  onContextSelect?: (id: string) => void;
  contextRightSlot?: React.ReactNode;
  titleRightSlot?: React.ReactNode;
  bottomSlot?: React.ReactNode;
}

const RightPanelTitleArea: React.FC<RightPanelTitleAreaProps> = ({
  title,
  titleNode,
  contextItems,
  activeContextId,
  onContextSelect,
  contextRightSlot,
  titleRightSlot,
  bottomSlot,
}) => {
  const shouldRenderContextBar = Boolean(contextItems && activeContextId && onContextSelect);

  return (
    <div className="right-panel-title-area">
      <div className="right-panel-title-row">
        {titleNode || <h3 className="right-panel-title">{title}</h3>}
        {titleRightSlot && <div className="right-panel-title-row-right">{titleRightSlot}</div>}
      </div>
      {shouldRenderContextBar && (
        <RightPanelContextBar
          items={contextItems!}
          activeId={activeContextId!}
          onSelect={onContextSelect!}
          rightSlot={contextRightSlot}
        />
      )}
      {bottomSlot && <div className="right-panel-title-bottom">{bottomSlot}</div>}
    </div>
  );
};

export default RightPanelTitleArea;
