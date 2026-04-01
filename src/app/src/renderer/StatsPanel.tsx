import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { RefreshIcon, XIcon } from './components/Icons';
import { onServerUrlChange, serverConfig } from './utils/serverConfig';

type BucketMap = Record<string, {
  requests?: number;
  input_tokens?: number;
  output_tokens?: number;
}>;

interface LifetimeStats {
  requests?: number;
  input_tokens?: number;
  output_tokens?: number;
  started_at?: string;
  updated_at?: string;
  bucket_timezone?: string;
  persistence_path?: string;
  by_day?: BucketMap;
  by_hour?: BucketMap;
}

interface StatsPanelProps {
  searchQuery: string;
}

interface UsagePoint {
  bucket: string;
  label: string;
  inputTokens: number;
  outputTokens: number;
  totalTokens: number;
  requests: number;
}

type BucketMode = 'day' | 'hour';
type DayRangePreset = '7d' | '30d' | '90d' | '365d' | 'all';

const DAY_RANGE_PRESETS: Array<{ key: DayRangePreset; label: string; days: number | null }> = [
  { key: '7d', label: 'Past 7 days', days: 7 },
  { key: '30d', label: 'Past 30 days', days: 30 },
  { key: '90d', label: 'N days', days: 90 },
  { key: '365d', label: 'Past year', days: 365 },
  { key: 'all', label: 'All time', days: null },
];

const HOURLY_SLOT_COUNT = 24;
const SYSTEM_USES_12_HOUR_CLOCK = new Intl.DateTimeFormat(undefined, {
  hour: 'numeric',
}).resolvedOptions().hour12 === true;

const StatsPanel: React.FC<StatsPanelProps> = ({ searchQuery }) => {
  const [lifetimeStats, setLifetimeStats] = useState<LifetimeStats | null>(null);
  const [bucketMode, setBucketMode] = useState<BucketMode>('day');
  const [dayRangePreset, setDayRangePreset] = useState<DayRangePreset>('30d');
  const [customDayCount, setCustomDayCount] = useState(90);
  const [selectedDay, setSelectedDay] = useState('');
  const [isExpanded, setIsExpanded] = useState(false);
  const [isLoading, setIsLoading] = useState(true);
  const [isRefreshing, setIsRefreshing] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const fetchStats = useCallback(async (refresh = false) => {
    if (refresh) {
      setIsRefreshing(true);
    } else {
      setIsLoading(true);
    }

    try {
      const response = await serverConfig.fetch('/stats');
      if (!response.ok) {
        throw new Error(`Stats request failed with ${response.status}`);
      }

      const data = await response.json();
      setLifetimeStats(data.lifetime ?? null);
      setError(null);
    } catch (fetchError) {
      console.error('Failed to load lifetime stats:', fetchError);
      setError('Unable to load server usage stats.');
    } finally {
      setIsLoading(false);
      setIsRefreshing(false);
    }
  }, []);

  useEffect(() => {
    fetchStats();

    const intervalId = window.setInterval(() => {
      fetchStats(true);
    }, 30000);

    const handleInferenceComplete = () => {
      fetchStats(true);
    };

    window.addEventListener('inference-complete', handleInferenceComplete);
    const unsubscribe = onServerUrlChange(() => {
      fetchStats();
    });

    return () => {
      window.clearInterval(intervalId);
      window.removeEventListener('inference-complete', handleInferenceComplete);
      unsubscribe();
    };
  }, [fetchStats]);

  const availableDays = useMemo(() => {
    return Object.keys(lifetimeStats?.by_day ?? {}).sort((a, b) => a.localeCompare(b));
  }, [lifetimeStats]);

  useEffect(() => {
    if (!availableDays.length) {
      if (selectedDay) {
        setSelectedDay('');
      }
      return;
    }

    if (!selectedDay || !availableDays.includes(selectedDay)) {
      setSelectedDay(availableDays[availableDays.length - 1]);
    }
  }, [availableDays, selectedDay]);

  const dayPoints = useMemo<UsagePoint[]>(() => {
    const entries = Object.entries(lifetimeStats?.by_day ?? {}).sort(([a], [b]) => a.localeCompare(b));
    const dayLimit = resolveDayLimit(dayRangePreset, customDayCount);
    const trimmed = dayLimit === null ? entries : entries.slice(-dayLimit);

    return trimmed.map(([bucket, value]) => makeUsagePoint(bucket, formatDayLabel(bucket), value));
  }, [customDayCount, dayRangePreset, lifetimeStats]);

  const hourlyPoints = useMemo<UsagePoint[]>(() => {
    if (!selectedDay) {
      return [];
    }

    const hours = new Map(Object.entries(lifetimeStats?.by_hour ?? {}));
    const points: UsagePoint[] = [];

    for (let hour = 0; hour < HOURLY_SLOT_COUNT; hour += 1) {
      const bucket = `${selectedDay}T${String(hour).padStart(2, '0')}:00:00`;
      const value = hours.get(bucket) ?? {};
      points.push(makeUsagePoint(bucket, formatHourTickLabel(hour), value));
    }

    return points;
  }, [lifetimeStats, selectedDay]);

  const basePoints = bucketMode === 'day' ? dayPoints : hourlyPoints;

  const filteredPoints = useMemo(() => {
    const trimmed = searchQuery.trim().toLowerCase();
    if (!trimmed) {
      return basePoints;
    }

    return basePoints.filter((point) =>
      point.bucket.toLowerCase().includes(trimmed) ||
      point.label.toLowerCase().includes(trimmed)
    );
  }, [basePoints, searchQuery]);

  const chartPoints = filteredPoints.length > 0 ? filteredPoints : basePoints;

  const chartSummary = useMemo(() => {
    const totals = chartPoints.reduce((acc, point) => {
      acc.input += point.inputTokens;
      acc.output += point.outputTokens;
      acc.requests += point.requests;
      return acc;
    }, { input: 0, output: 0, requests: 0 });

    const totalTokens = totals.input + totals.output;

    return {
      totalTokens,
      input: totals.input,
      output: totals.output,
      requests: totals.requests,
      avgTokensPerRequest: totals.requests > 0 ? totalTokens / totals.requests : 0,
    };
  }, [chartPoints]);

  const lifetimeSummary = useMemo(() => {
    const totalInput = lifetimeStats?.input_tokens ?? 0;
    const totalOutput = lifetimeStats?.output_tokens ?? 0;
    const requests = lifetimeStats?.requests ?? 0;
    const totalTokens = totalInput + totalOutput;

    return {
      requests,
      totalInput,
      totalOutput,
      totalTokens,
      avgTokensPerRequest: requests > 0 ? totalTokens / requests : 0,
    };
  }, [lifetimeStats]);

  const valueSummary = chartSummary;

  const recentRows = useMemo(() => {
    return [...chartPoints].reverse().slice(0, 8);
  }, [chartPoints]);

  const utilizationSummary = useMemo(() => {
    const allDayPoints = Object.entries(lifetimeStats?.by_day ?? {})
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([bucket, value]) => makeUsagePoint(bucket, formatDayLabel(bucket), value));
    const allHourPoints = Object.entries(lifetimeStats?.by_hour ?? {})
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([bucket, value]) => makeUsagePoint(bucket, bucket, value));

    const activeDays = allDayPoints.filter((point) => point.totalTokens > 0).length;
    const activeHours = allHourPoints.filter((point) => point.totalTokens > 0).length;
    const peakDay = allDayPoints.reduce<UsagePoint | null>((best, point) => {
      if (!best || point.totalTokens > best.totalTokens) {
        return point;
      }
      return best;
    }, null);
    const peakHour = allHourPoints.reduce<UsagePoint | null>((best, point) => {
      if (!best || point.totalTokens > best.totalTokens) {
        return point;
      }
      return best;
    }, null);

    return {
      activeDays,
      activeHours,
      avgTokensPerActiveDay: activeDays > 0 ? lifetimeSummary.totalTokens / activeDays : 0,
      peakDayLabel: peakDay ? formatDateHeading(peakDay.bucket) : 'No activity yet',
      peakDayTokens: peakDay?.totalTokens ?? 0,
      peakHourLabel: peakHour ? formatHourBucketHeading(peakHour.bucket) : 'No activity yet',
      peakHourTokens: peakHour?.totalTokens ?? 0,
    };
  }, [lifetimeStats, lifetimeSummary.totalTokens]);

  const renderRangeControls = () => (
    bucketMode === 'day' ? (
      <div className="stats-controls">
        <div className="stats-chip-group">
          {DAY_RANGE_PRESETS.map((preset) => (
            <button
              key={preset.key}
              type="button"
              className={`stats-chip ${dayRangePreset === preset.key ? 'active' : ''}`}
              onClick={() => setDayRangePreset(preset.key)}
            >
              {preset.label}
            </button>
          ))}
        </div>
        {dayRangePreset === '90d' && (
          <label className="stats-inline-control">
            <span>Days</span>
            <input
              type="number"
              min="1"
              max="3650"
              value={customDayCount}
              onChange={(event) => setCustomDayCount(clampDayCount(event.target.value))}
            />
          </label>
        )}
      </div>
    ) : (
      <div className="stats-controls">
        <label className="stats-inline-control">
          <span>Day</span>
          <input
            type="date"
            value={selectedDay}
            min={availableDays[0] ?? ''}
            max={availableDays[availableDays.length - 1] ?? ''}
            onChange={(event) => setSelectedDay(event.target.value)}
          />
        </label>
      </div>
    )
  );

  return (
    <div className="stats-panel">
      <div className="stats-hero">
        <div className="stats-hero-copy">
          <div className="stats-kicker">SERVER LIFETIME</div>
          <h2>Usage trends</h2>
          <p>
            See whether the local accelerator is being used steadily or sitting idle,
            with day-range and single-day hourly views.
          </p>
        </div>
        <div className="stats-hero-actions">
          <button
            className="stats-expand-btn"
            onClick={() => setIsExpanded(true)}
            title="Open larger chart"
            aria-label="Open larger chart"
          >
            Zoom
          </button>
          <button
            className={`stats-refresh-btn ${isRefreshing ? 'spinning' : ''}`}
            onClick={() => fetchStats(true)}
            title="Refresh stats"
            aria-label="Refresh stats"
          >
            <RefreshIcon />
          </button>
        </div>
      </div>

      <div className="stats-summary-grid">
        <div className="stats-summary-card">
          <span className="stats-summary-label">Lifetime tokens</span>
          <strong>{formatNumber(lifetimeSummary.totalTokens)}</strong>
          <span className="stats-summary-meta">All persisted prompt and completion tokens</span>
        </div>
        <div className="stats-summary-card">
          <span className="stats-summary-label">{bucketMode === 'day' ? 'Selected range' : 'Selected day'}</span>
          <strong>{formatNumber(valueSummary.totalTokens)}</strong>
          <span className="stats-summary-meta">
            {bucketMode === 'day' ? 'Visible token volume in chart' : `${formatDateHeading(selectedDay)} token flow`}
          </span>
        </div>
        <div className="stats-summary-card">
          <span className="stats-summary-label">Lifetime requests</span>
          <strong>{formatNumber(lifetimeSummary.requests)}</strong>
          <span className="stats-summary-meta">{formatCompactNumber(lifetimeSummary.avgTokensPerRequest)} avg tokens per request</span>
        </div>
        <div className="stats-summary-card">
          <span className="stats-summary-label">Output share</span>
          <strong>{formatPercent(lifetimeSummary.totalOutput, lifetimeSummary.totalTokens)}</strong>
          <span className="stats-summary-meta">
            {formatNumber(lifetimeSummary.totalOutput)} output vs {formatNumber(lifetimeSummary.totalInput)} input
          </span>
        </div>
      </div>

      <div className="stats-chart-shell">
        <div className="stats-chart-toolbar">
          <div>
            <div className="stats-chart-title">
              {bucketMode === 'day' ? 'Token flow by day' : 'Token flow by hour'}
            </div>
            <div className="stats-chart-subtitle">
              {bucketMode === 'day'
                ? describeDayRange(dayRangePreset, customDayCount)
                : `24-hour view for ${formatDateHeading(selectedDay)}`}
              {searchQuery.trim().length > 0 ? ` matching "${searchQuery.trim()}"` : ''}
            </div>
          </div>
          <div className="stats-bucket-toggle">
            <button
              type="button"
              className={bucketMode === 'day' ? 'active' : ''}
              onClick={() => setBucketMode('day')}
            >
              Per day
            </button>
            <button
              type="button"
              className={bucketMode === 'hour' ? 'active' : ''}
              onClick={() => setBucketMode('hour')}
            >
              Per hour
            </button>
          </div>
        </div>

        {renderRangeControls()}

        <ChartSection
          points={chartPoints}
          mode={bucketMode}
          isLoading={isLoading}
          error={error}
          expanded={false}
          onExpand={() => setIsExpanded(true)}
        />
      </div>

      <div className="stats-detail-grid">
        <div className="stats-detail-card">
          <div className="stats-detail-heading">Current view</div>
          <div className="stats-detail-subheading">
            Quick read on visible throughput and usage density
          </div>
          <div className="stats-insight-grid">
            <div className="stats-insight-pill">
              <span>Visible requests</span>
              <strong>{formatNumber(chartSummary.requests)}</strong>
            </div>
            <div className="stats-insight-pill">
              <span>Input tokens</span>
              <strong>{formatCompactNumber(chartSummary.input)}</strong>
            </div>
            <div className="stats-insight-pill">
              <span>Output tokens</span>
              <strong>{formatCompactNumber(chartSummary.output)}</strong>
            </div>
            <div className="stats-insight-pill">
              <span>Avg/request</span>
              <strong>{formatCompactNumber(chartSummary.avgTokensPerRequest)}</strong>
            </div>
          </div>
        </div>

        <div className="stats-detail-card">
          <div className="stats-detail-heading">Utilization</div>
          <div className="stats-detail-subheading">
            High-signal indicators for whether the local accelerator is being put to work
          </div>
          <div className="stats-meta-list">
            <div className="stats-meta-row">
              <span>Active days</span>
              <strong>{formatNumber(utilizationSummary.activeDays)}</strong>
            </div>
            <div className="stats-meta-row">
              <span>Active hours</span>
              <strong>{formatNumber(utilizationSummary.activeHours)}</strong>
            </div>
            <div className="stats-meta-row">
              <span>Avg tokens / active day</span>
              <strong>{formatCompactNumber(utilizationSummary.avgTokensPerActiveDay)}</strong>
            </div>
            <div className="stats-meta-row">
              <span>Peak day</span>
              <strong>{utilizationSummary.peakDayLabel}</strong>
              <small>{formatCompactNumber(utilizationSummary.peakDayTokens)} tokens</small>
            </div>
            <div className="stats-meta-row">
              <span>Peak hour</span>
              <strong>{utilizationSummary.peakHourLabel}</strong>
              <small>{formatCompactNumber(utilizationSummary.peakHourTokens)} tokens</small>
            </div>
          </div>
        </div>
      </div>

      <div className="stats-detail-card">
        <div className="stats-detail-heading">
          Recent {bucketMode === 'day' ? 'days' : 'hours'}
        </div>
        <div className="stats-detail-subheading">
          Useful for spotting bursts, idle windows, and uneven workload distribution
        </div>
        {recentRows.length === 0 ? (
          <div className="stats-empty-state compact">No matching buckets.</div>
        ) : (
          <div className="stats-table">
            {recentRows.map((row) => (
              <div key={row.bucket} className="stats-table-row">
                <div className="stats-table-period">
                  <span>{row.label}</span>
                  <small>{row.requests} req</small>
                </div>
                <div className="stats-table-values">
                  <span>{formatCompactNumber(row.inputTokens)} in</span>
                  <span>{formatCompactNumber(row.outputTokens)} out</span>
                  <strong>{formatCompactNumber(row.totalTokens)} total</strong>
                </div>
              </div>
            ))}
          </div>
        )}
      </div>

      {isExpanded && (
        <div className="stats-overlay" onMouseDown={(event) => {
          if (event.target === event.currentTarget) {
            setIsExpanded(false);
          }
        }}>
          <div className="stats-overlay-card" onMouseDown={(event) => event.stopPropagation()}>
            <div className="stats-overlay-header">
              <div className="stats-overlay-header-main">
                <div>
                <div className="stats-chart-title">
                  {bucketMode === 'day' ? 'Expanded daily token flow' : 'Expanded hourly token flow'}
                </div>
                <div className="stats-chart-subtitle">
                  {bucketMode === 'day'
                    ? describeDayRange(dayRangePreset, customDayCount)
                    : `24-hour view for ${formatDateHeading(selectedDay)}`}
                </div>
                </div>
                <div className="stats-bucket-toggle overlay">
                  <button
                    type="button"
                    className={bucketMode === 'day' ? 'active' : ''}
                    onClick={() => setBucketMode('day')}
                  >
                    Per day
                  </button>
                  <button
                    type="button"
                    className={bucketMode === 'hour' ? 'active' : ''}
                    onClick={() => setBucketMode('hour')}
                  >
                    Per hour
                  </button>
                </div>
              </div>
              <button
                className="stats-overlay-close"
                onClick={() => setIsExpanded(false)}
                title="Close expanded chart"
                aria-label="Close expanded chart"
              >
                <XIcon size={16} strokeWidth={2.2} />
              </button>
            </div>
            {renderRangeControls()}
            <ChartSection
              points={chartPoints}
              mode={bucketMode}
              isLoading={isLoading}
              error={error}
              expanded={true}
            />
          </div>
        </div>
      )}
    </div>
  );
};

interface ChartSectionProps {
  points: UsagePoint[];
  mode: BucketMode;
  isLoading: boolean;
  error: string | null;
  expanded: boolean;
  onExpand?: () => void;
}

const ChartSection: React.FC<ChartSectionProps> = ({
  points,
  mode,
  isLoading,
  error,
  expanded,
  onExpand,
}) => {
  const [hoveredPointIndex, setHoveredPointIndex] = useState<number | null>(null);
  const shouldShowSeriesMarkers = points.length <= 3;
  const chartGeometry = useMemo(() => {
    const width = 920;
    const height = expanded ? 430 : 260;
    const paddingX = expanded ? 34 : 22;
    const paddingTop = 18;
    const paddingBottom = expanded ? 64 : 38;
    const innerWidth = width - paddingX * 2;
    const innerHeight = height - paddingTop - paddingBottom;
    const maxTokens = Math.max(...points.map((point) => point.totalTokens), 1);

    const xForIndex = (index: number) => {
      if (points.length <= 1) {
        return width / 2;
      }
      if (points.length === 2) {
        return width * (index === 0 ? 0.32 : 0.68);
      }
      return paddingX + (index / (points.length - 1)) * innerWidth;
    };

    const hoverBandForIndex = (index: number) => {
      if (points.length <= 1) {
        return {
          x: paddingX,
          width: width - paddingX * 2,
        };
      }

      const currentX = xForIndex(index);
      const prevX = index > 0 ? xForIndex(index - 1) : currentX - (xForIndex(index + 1) - currentX);
      const nextX = index < points.length - 1 ? xForIndex(index + 1) : currentX + (currentX - xForIndex(index - 1));
      const left = index === 0 ? paddingX : (prevX + currentX) / 2;
      const right = index === points.length - 1 ? width - paddingX : (currentX + nextX) / 2;

      return {
        x: left,
        width: Math.max(12, right - left),
      };
    };

    const yForValue = (value: number) => {
      const normalized = value / maxTokens;
      return paddingTop + innerHeight - normalized * innerHeight;
    };

    const hoverZoneForPoint = (point: UsagePoint) => {
      if (point.totalTokens <= 0) {
        return null;
      }

      const ys = [
        yForValue(point.inputTokens),
        yForValue(point.outputTokens),
        yForValue(point.totalTokens),
      ];
      const top = Math.max(paddingTop, Math.min(...ys) - 18);
      const bottom = Math.min(height - paddingBottom, Math.max(...ys) + 18);

      return {
        y: top,
        height: Math.max(28, bottom - top),
      };
    };

    const buildPath = (values: number[]) => values.map((value, index) => {
      const x = xForIndex(index);
      const y = yForValue(value);
      return `${index === 0 ? 'M' : 'L'} ${x.toFixed(2)} ${y.toFixed(2)}`;
    }).join(' ');

    const inputPath = buildPath(points.map((point) => point.inputTokens));
    const outputPath = buildPath(points.map((point) => point.outputTokens));

    const gridLines = 4;
    const yTicks = Array.from({ length: gridLines + 1 }, (_, index) => {
      const value = Math.round((maxTokens / gridLines) * (gridLines - index));
      const y = paddingTop + (innerHeight / gridLines) * index;
      return { value, y };
    });

    return {
      width,
      height,
      paddingX,
      paddingBottom,
      inputPath,
      outputPath,
      xForIndex,
      hoverBandForIndex,
      hoverZoneForPoint,
      yTicks,
      yForValue,
    };
  }, [expanded, points]);

  return (
    <>
      <div className="stats-chart-legend">
        <span><i className="stats-swatch input" />Input</span>
        <span><i className="stats-swatch output" />Output</span>
        {!expanded && onExpand && (
          <button className="stats-link-button" onClick={onExpand}>
            Open large chart
          </button>
        )}
      </div>

      <div className={`stats-chart-frame ${expanded ? 'expanded' : ''}`}>
        {isLoading ? (
          <div className="stats-empty-state">Loading stats…</div>
        ) : error ? (
          <div className="stats-empty-state error">{error}</div>
        ) : points.length === 0 ? (
          <div className="stats-empty-state">No bucketed token data yet.</div>
        ) : (
          <>
            <svg className="stats-chart" viewBox={`0 0 ${chartGeometry.width} ${chartGeometry.height}`} preserveAspectRatio="none">
              <defs>
                <linearGradient id={expanded ? 'stats-area-gradient-large' : 'stats-area-gradient'} x1="0" y1="0" x2="0" y2="1">
                  <stop offset="0%" stopColor="#f7b500" stopOpacity="0.45" />
                  <stop offset="100%" stopColor="#f7b500" stopOpacity="0.02" />
                </linearGradient>
              </defs>

              {chartGeometry.yTicks.map((tick) => (
                <g key={tick.y}>
                  <line
                    className="stats-grid-line"
                    x1={chartGeometry.paddingX}
                    y1={tick.y}
                    x2={chartGeometry.width - chartGeometry.paddingX}
                    y2={tick.y}
                  />
                  <text x="4" y={tick.y + 4} className="stats-grid-label">
                    {formatCompactNumber(tick.value)}
                  </text>
                </g>
              ))}

              {points.map((point, index) => (
                <line
                  key={`x-grid-${point.bucket}`}
                  className={`stats-grid-line vertical ${shouldShowXAxisLabel(mode, index, points.length) ? 'major' : ''}`}
                  x1={chartGeometry.xForIndex(index)}
                  y1={chartGeometry.yTicks[0].y}
                  x2={chartGeometry.xForIndex(index)}
                  y2={chartGeometry.height - chartGeometry.paddingBottom}
                />
              ))}

              <path className="stats-line input" d={chartGeometry.inputPath} />
              <path className="stats-line output" d={chartGeometry.outputPath} />

              {points.map((point, index) => (
                <g key={point.bucket}>
                  {chartGeometry.hoverZoneForPoint(point) && (
                    <rect
                      className="stats-hover-band"
                      x={chartGeometry.hoverBandForIndex(index).x}
                      y={chartGeometry.hoverZoneForPoint(point)!.y}
                      width={chartGeometry.hoverBandForIndex(index).width}
                      height={chartGeometry.hoverZoneForPoint(point)!.height}
                      onMouseEnter={() => setHoveredPointIndex(index)}
                      onMouseLeave={() => setHoveredPointIndex((current) => (current === index ? null : current))}
                    />
                  )}
                  {shouldShowSeriesMarkers && (
                    <>
                      <circle
                        className="stats-series-point input"
                        cx={chartGeometry.xForIndex(index)}
                        cy={chartGeometry.yForValue(point.inputTokens)}
                        r={expanded ? 4.2 : 3.4}
                      />
                      <circle
                        className="stats-series-point output"
                        cx={chartGeometry.xForIndex(index)}
                        cy={chartGeometry.yForValue(point.outputTokens)}
                        r={expanded ? 4.2 : 3.4}
                      />
                    </>
                  )}
                  <circle
                    className="stats-point-hitbox"
                    cx={chartGeometry.xForIndex(index)}
                    cy={chartGeometry.yForValue(point.totalTokens)}
                    r={expanded ? 11 : 9}
                  />
                  {shouldShowXAxisLabel(mode, index, points.length) && (
                    <text
                      className={`stats-x-label ${expanded ? 'expanded' : ''}`}
                      x={chartGeometry.xForIndex(index)}
                      y={chartGeometry.height - 11}
                      textAnchor="middle"
                    >
                      {point.label}
                    </text>
                  )}
                </g>
              ))}
            </svg>
            {hoveredPointIndex !== null && points[hoveredPointIndex] && (
              (() => {
                const hoveredPoint = points[hoveredPointIndex];
                const xPercent = (chartGeometry.xForIndex(hoveredPointIndex) / chartGeometry.width) * 100;
                const yPercent = (chartGeometry.yForValue(hoveredPoint.totalTokens) / chartGeometry.height) * 100;
                const horizontalClass = xPercent > 82 ? 'align-right' : xPercent < 18 ? 'align-left' : '';
                const verticalClass = chartGeometry.yForValue(hoveredPoint.totalTokens) < 92 ? 'below' : '';

                return (
                  <div
                    className={`stats-hover-card ${expanded ? 'expanded' : ''} ${horizontalClass} ${verticalClass}`}
                    style={{
                      left: `${xPercent}%`,
                      top: `${yPercent}%`,
                    }}
                  >
                    <div className="stats-hover-title">{hoveredPoint.bucket}</div>
                    <div className="stats-hover-row total"><span><i className="stats-hover-swatch total" />Total</span><strong>{formatNumber(hoveredPoint.totalTokens)}</strong></div>
                    <div className="stats-hover-row input"><span><i className="stats-hover-swatch input" />Input</span><strong>{formatNumber(hoveredPoint.inputTokens)}</strong></div>
                    <div className="stats-hover-row output"><span><i className="stats-hover-swatch output" />Output</span><strong>{formatNumber(hoveredPoint.outputTokens)}</strong></div>
                    <div className="stats-hover-row requests"><span>Requests</span><strong>{formatNumber(hoveredPoint.requests)}</strong></div>
                  </div>
                );
              })()
            )}
          </>
        )}
      </div>
    </>
  );
};

function makeUsagePoint(bucket: string, label: string, value: { requests?: number; input_tokens?: number; output_tokens?: number }): UsagePoint {
  const inputTokens = value.input_tokens ?? 0;
  const outputTokens = value.output_tokens ?? 0;

  return {
    bucket,
    label,
    inputTokens,
    outputTokens,
    totalTokens: inputTokens + outputTokens,
    requests: value.requests ?? 0,
  };
}

function resolveDayLimit(preset: DayRangePreset, customDayCount: number): number | null {
  const range = DAY_RANGE_PRESETS.find((entry) => entry.key === preset);
  if (!range) {
    return 30;
  }
  if (range.days !== null) {
    return range.days;
  }
  if (preset === 'all') {
    return null;
  }
  return customDayCount;
}

function describeDayRange(preset: DayRangePreset, customDayCount: number): string {
  if (preset === '90d') {
    return `Past ${customDayCount} days`;
  }
  return DAY_RANGE_PRESETS.find((entry) => entry.key === preset)?.label ?? 'Past 30 days';
}

function clampDayCount(value: string): number {
  const parsed = Number.parseInt(value, 10);
  if (Number.isNaN(parsed)) {
    return 1;
  }
  return Math.max(1, Math.min(3650, parsed));
}

function shouldShowXAxisLabel(mode: BucketMode, index: number, totalCount: number): boolean {
  if (totalCount <= 8) {
    return true;
  }
  if (mode === 'hour') {
    return index % 2 === 0 || index === totalCount - 1;
  }
  const step = Math.max(1, Math.ceil(totalCount / 6));
  return index % step === 0 || index === totalCount - 1;
}

function formatNumber(value: number): string {
  return value.toLocaleString();
}

function formatCompactNumber(value: number): string {
  return new Intl.NumberFormat(undefined, {
    notation: 'compact',
    maximumFractionDigits: value >= 100 ? 0 : 1,
  }).format(value);
}

function formatPercent(value: number, total: number): string {
  if (total <= 0) {
    return '0%';
  }
  return `${((value / total) * 100).toFixed(1)}%`;
}

function formatDayLabel(bucket: string): string {
  const date = new Date(`${bucket}T00:00:00`);
  if (Number.isNaN(date.getTime())) {
    return bucket;
  }

  return new Intl.DateTimeFormat(undefined, {
    month: 'short',
    day: 'numeric',
  }).format(date);
}

function formatHourTickLabel(hour: number): string {
  const date = new Date(Date.UTC(2026, 0, 1, hour, 0, 0));
  if (SYSTEM_USES_12_HOUR_CLOCK) {
    return new Intl.DateTimeFormat(undefined, {
      hour: 'numeric',
      hour12: true,
      timeZone: 'UTC',
    }).format(date);
  }

  return new Intl.DateTimeFormat(undefined, {
    hour: '2-digit',
    hour12: false,
    timeZone: 'UTC',
  }).format(date);
}

function formatDateHeading(value: string): string {
  if (!value) {
    return 'No day selected';
  }
  const date = new Date(`${value}T00:00:00`);
  if (Number.isNaN(date.getTime())) {
    return value;
  }
  return new Intl.DateTimeFormat(undefined, {
    weekday: 'short',
    month: 'short',
    day: 'numeric',
    year: 'numeric',
  }).format(date);
}

function formatHourBucketHeading(value: string): string {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    return value;
  }
  return new Intl.DateTimeFormat(undefined, {
    month: 'short',
    day: 'numeric',
    hour: 'numeric',
  }).format(date);
}

export default StatsPanel;
