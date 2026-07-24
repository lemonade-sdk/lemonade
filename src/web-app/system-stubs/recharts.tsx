import React from 'react';

type StubProps = {
  children?: React.ReactNode;
  className?: string;
  style?: React.CSSProperties;
  [key: string]: unknown;
};

export const ResponsiveContainer: React.FC<StubProps> = ({ children, className, style }) => (
  <div className={className} style={{ width: '100%', minHeight: 120, ...style }}>
    {children}
  </div>
);

export const AreaChart: React.FC<StubProps> = ({ children }) => (
  <svg role="img" aria-label="Chart unavailable in system package build" width="100%" height="120">
    <text x="12" y="24" fill="currentColor" fontSize="12">Chart unavailable in system package build.</text>
    {children}
  </svg>
);

export const Area: React.FC<StubProps> = () => null;
export const YAxis: React.FC<StubProps> = () => null;
export const CartesianGrid: React.FC<StubProps> = () => null;
export const Tooltip: React.FC<StubProps> = () => null;
