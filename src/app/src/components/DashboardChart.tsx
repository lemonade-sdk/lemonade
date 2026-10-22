import React, { useId, useMemo, useState } from 'react';

const WIDTH = 1000;
const TOP = 4;
const BOTTOM = 96;

const CHART_TOOLTIP_STYLE: React.CSSProperties = {
  position: 'absolute',
  top: 0,
  zIndex: 1,
  pointerEvents: 'none',
  background: '#1d1b16',
  border: '1px solid rgba(255,245,220,0.1)',
  borderRadius: 8,
  fontSize: 12,
  padding: '6px 10px',
  whiteSpace: 'nowrap',
};

export interface DashboardChartProps {
  data: Record<string, number>[];
  series: { key: string; color: string; name?: string }[];
  height?: number;
  unit?: string;
}

type Point = { x: number; y: number };

function traceSegments(values: Record<string, number>[], key: string, yMax: number): Point[][] {
  const segments: Point[][] = [];
  let segment: Point[] = [];
  values.forEach((sample, index) => {
    const value = sample[key];
    if (!Number.isFinite(value)) {
      if (segment.length) segments.push(segment);
      segment = [];
      return;
    }
    segment.push({
      x: values.length === 1 ? WIDTH / 2 : index * WIDTH / (values.length - 1),
      y: BOTTOM - Math.max(0, value) / yMax * (BOTTOM - TOP),
    });
  });
  if (segment.length) segments.push(segment);
  return segments;
}

function linePath(points: Point[]): string {
  if (!points.length) return '';
  const path = [`M${points[0].x},${points[0].y}`];
  if (points.length === 1) return path[0];
  if (points.length === 2) return `${path[0]} L${points[1].x},${points[1].y}`;

  // Monotone cubic interpolation keeps curves smooth without overshooting.
  const tangents = new Array<number>(points.length);
  for (let i = 1; i < points.length - 1; i++) {
    const prev = points[i - 1];
    const curr = points[i];
    const next = points[i + 1];
    const h0 = curr.x - prev.x;
    const h1 = next.x - curr.x;
    const s0 = (curr.y - prev.y) / h0;
    const s1 = (next.y - curr.y) / h1;
    const weighted = (s0 * h1 + s1 * h0) / (h0 + h1);
    tangents[i] = (Math.sign(s0) + Math.sign(s1))
      * Math.min(Math.abs(s0), Math.abs(s1), Math.abs(weighted) / 2) || 0;
  }
  const first = points[0];
  const second = points[1];
  const last = points[points.length - 1];
  const penultimate = points[points.length - 2];
  tangents[0] = (3 * (second.y - first.y) / (second.x - first.x) - tangents[1]) / 2;
  tangents[tangents.length - 1] = (3 * (last.y - penultimate.y) / (last.x - penultimate.x)
    - tangents[tangents.length - 2]) / 2;

  for (let i = 1; i < points.length; i++) {
    const a = points[i - 1];
    const b = points[i];
    const dx = (b.x - a.x) / 3;
    path.push(`C${a.x + dx},${a.y + dx * tangents[i - 1]} `
      + `${b.x - dx},${b.y - dx * tangents[i]} ${b.x},${b.y}`);
  }
  return path.join(' ');
}

const DashboardChart: React.FC<DashboardChartProps> = ({
  data,
  series,
  height = 80,
  unit = '',
}) => {
  const gradientPrefix = useId().replace(/:/g, '');
  const [hovered, setHovered] = useState<number | null>(null);
  const yMax = useMemo(() => {
    let maximum = 0;
    for (const sample of data) {
      for (const item of series) {
        const value = sample[item.key];
        if (Number.isFinite(value) && value > maximum) maximum = value;
      }
    }
    return maximum > 0 ? maximum * 1.05 : 1;
  }, [data, series]);

  const paths = useMemo(() => series.map(item => ({
    ...item,
    segments: traceSegments(data, item.key, yMax),
  })), [data, series, yMax]);

  const visibleHover = hovered !== null && hovered < data.length ? hovered : null;
  const hoverX = visibleHover === null ? 0 : data.length === 1
    ? 50 : 100 * visibleHover / (data.length - 1);

  return (
    <div style={{ width: '100%', height, position: 'relative', minWidth: 0 }}>
      <svg
        width="100%"
        height="100%"
        viewBox={`0 0 ${WIDTH} 100`}
        preserveAspectRatio="none"
        role="img"
        aria-label="Metric history chart"
        onPointerMove={event => {
          if (!data.length) return;
          const rect = event.currentTarget.getBoundingClientRect();
          if (!rect.width) return;
          const position = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
          setHovered(Math.round(position * (data.length - 1)));
        }}
        onPointerLeave={() => setHovered(null)}
      >
        <defs>
          {series.map(item => (
            <linearGradient key={item.key} id={`${gradientPrefix}-${item.key}`} x1="0" y1="0" x2="0" y2="1">
              <stop offset="0%" stopColor={item.color} stopOpacity={0.3} />
              <stop offset="100%" stopColor={item.color} stopOpacity={0} />
            </linearGradient>
          ))}
        </defs>
        {[0, 1, 2, 3, 4].map(index => (
          <line
            key={index}
            x1="0" x2={WIDTH}
            y1={TOP + index * (BOTTOM - TOP) / 4}
            y2={TOP + index * (BOTTOM - TOP) / 4}
            stroke="rgba(255,245,220,0.04)"
            strokeDasharray="3 3"
            vectorEffect="non-scaling-stroke"
          />
        ))}
        {paths.map(item => item.segments.map((points, index) => {
          const line = linePath(points);
          const area = `${line} L${points[points.length - 1].x},${BOTTOM} L${points[0].x},${BOTTOM} Z`;
          return (
            <g key={`${item.key}-${index}`}>
              {points.length > 1 && (
                <>
                  <path d={area} fill={`url(#${gradientPrefix}-${item.key})`} />
                  <path d={line} fill="none" stroke={item.color} strokeWidth="1" strokeLinecap="round" strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
                </>
              )}
              {points.length === 1 && (
                <line x1={points[0].x} y1={points[0].y} x2={points[0].x + 0.1} y2={points[0].y}
                  stroke={item.color} strokeWidth="1.5" strokeLinecap="round" vectorEffect="non-scaling-stroke" />
              )}
            </g>
          );
        }))}
        {visibleHover !== null && (
          <line
            x1={hoverX * 10} x2={hoverX * 10}
            y1={TOP} y2={BOTTOM}
            stroke="rgba(255,245,220,0.3)"
            strokeDasharray="3 3"
            strokeWidth="0.75"
            vectorEffect="non-scaling-stroke"
          />
        )}
      </svg>
      {visibleHover !== null && (
        <div style={{
          ...CHART_TOOLTIP_STYLE,
          left: `${hoverX}%`,
          transform: `translateX(${hoverX < 15 ? '0' : hoverX > 85 ? '-100%' : '-50%'})`,
        }}>
          {series.filter(item => Number.isFinite(data[visibleHover][item.key])).map(item => (
            <div key={item.key}>
              <span style={{ color: item.color }}>{item.name || item.key}</span>
              {': '}{data[visibleHover][item.key].toFixed(1)}{unit}
            </div>
          ))}
        </div>
      )}
    </div>
  );
};

export default React.memo(DashboardChart);
