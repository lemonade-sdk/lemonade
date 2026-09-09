import React from 'react';
import {
  AreaChart as RechartArea,
  Area,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
} from 'recharts';

const CHART_TOOLTIP_STYLE: React.CSSProperties = {
  background: '#1d1b16',
  border: '1px solid rgba(255,245,220,0.1)',
  borderRadius: 8,
  fontSize: 12,
  padding: '6px 10px',
};

export interface DashboardChartProps {
  data: Record<string, number>[];
  series: { key: string; color: string; name?: string }[];
  height?: number;
  unit?: string;
}

const DashboardChart: React.FC<DashboardChartProps> = ({
  data,
  series,
  height = 80,
  unit = '',
}) => (
  <ResponsiveContainer width="100%" height={height}>
    <RechartArea data={data} margin={{ top: 4, right: 0, bottom: 0, left: 0 }}>
      <defs>
        {series.map(s => (
          <linearGradient key={s.key} id={`grad-${s.key}`} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={s.color} stopOpacity={0.3} />
            <stop offset="100%" stopColor={s.color} stopOpacity={0} />
          </linearGradient>
        ))}
      </defs>
      <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,245,220,0.04)" vertical={false} />
      <YAxis hide domain={[0, 'auto']} />
      <Tooltip
        contentStyle={CHART_TOOLTIP_STYLE}
        labelStyle={{ display: 'none' }}
        formatter={(value) => [`${Number(value ?? 0).toFixed(1)}${unit}`, '']}
        isAnimationActive={false}
      />
      {series.map(s => (
        <Area
          key={s.key}
          type="monotone"
          dataKey={s.key}
          stroke={s.color}
          fill={`url(#grad-${s.key})`}
          strokeWidth={2}
          dot={false}
          isAnimationActive={false}
          name={s.name || s.key}
        />
      ))}
    </RechartArea>
  </ResponsiveContainer>
);

export default React.memo(DashboardChart);
