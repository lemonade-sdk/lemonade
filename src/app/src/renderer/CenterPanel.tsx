import React from 'react';
import AppMarketplace from './AppMarketplace';

interface CenterPanelProps {
  isVisible: boolean;
}

const CenterPanel: React.FC<CenterPanelProps> = ({ isVisible }) => {
  return <AppMarketplace isVisible={isVisible} />;
};

export default CenterPanel;

