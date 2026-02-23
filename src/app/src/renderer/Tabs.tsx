import React from 'react';
import TabsProvider, { useTabsContext } from './TabsContext';

interface TabTitlesProps {
  items: {
    id: string;
    label: string;
  }[]
}

interface TabContentProps {
  items: {
    id: string;
    content: React.ReactNode;
  }[]
}

interface TabsComposition {
  Labels: (props: TabTitlesProps) => React.ReactNode;
  Contents: (props: TabContentProps) => React.ReactNode;
}

type TabsProps = {
  children: React.ReactNode;
}

type TabsWrapper = (props: TabsProps) => React.ReactNode

const Tabs: TabsWrapper & TabsComposition = ({ children }) => {
  return <TabsProvider>{children}</TabsProvider>
}

Tabs.Labels = ({ items }) => {
  const { currentIndex, setCurrentIndex } = useTabsContext()
  return (
    <div role="tablist" className="settings-tab-controls">
      {items.map(({ id, label }, index) => (
        <button
          key={id}
          id={`tab-control-${id}`}
          className={`settings-tab-control ${currentIndex === index ? 'active' : ''}`}
          role="tab"
          aria-controls={`tab-content-${id}`}
          aria-selected={currentIndex === index}
          onClick={() => {
            setCurrentIndex(index)
          }}
        >
          {label}
        </button>
      ))}
    </div>
  )
}

Tabs.Contents = ({ items }) => {
  const { currentIndex } = useTabsContext()
  const { id, content } = items[currentIndex]
  return (
    <div
      key={id}
      id={`tab-content-${id}`}
      className="settings-tab-content"
      role="tabpanel"
      aria-labelledby={`tab-control-${id}`}
    >
      {content}
    </div>
  )
}

export default Tabs;
