import React, { useContext, useState } from 'react';

interface TabsContextProps {
  currentIndex: number;
  setCurrentIndex: React.Dispatch<React.SetStateAction<number>>;
}

interface TabsProviderProps {
  children: React.ReactNode;
}

const initialContext: TabsContextProps = {
  currentIndex: 0,
  setCurrentIndex: () => {},
}

const TabsContext = React.createContext<TabsContextProps>(initialContext);

const TabsProvider = ({ children }: TabsProviderProps) => {
  const [currentIndex, setCurrentIndex] = useState<number>(0)

  return (
    <TabsContext.Provider value={{ currentIndex, setCurrentIndex }}>
      {children}
    </TabsContext.Provider>
  )
}

export function useTabsContext(): TabsContextProps {
  const context = useContext(TabsContext);

  if (context === undefined) {
    throw new Error('useTabs must be used within a TabsProvider');
  }

  return context;
}

export default TabsProvider;
