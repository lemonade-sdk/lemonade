import React from 'react';
import type { SystemGpuStats } from '../api';
import type { Translate } from '../i18n';

type Props = {
  gpus: SystemGpuStats[];
  selectedId: string | null;
  onSelect: (id: string) => void;
  thresholdPercent?: number | null;
  t: Translate;
};

const reading = (value: number | null): number | null =>
  value != null && Number.isFinite(value) && value >= 0 ? value : null;

const gb = (value: number): string => (value >= 100 ? value.toFixed(0) : value.toFixed(1));

export const MonitorGpuVitals: React.FC<Props> = ({ gpus, selectedId, onSelect, thresholdPercent = null, t }) => {
  // lemond evicts when any single GPU crosses the threshold, so every card shows the same marker.
  const threshold = thresholdPercent != null && Number.isFinite(thresholdPercent)
    && thresholdPercent > 0 && thresholdPercent <= 100 ? thresholdPercent : null;
  return (
    <div className="dash2-gpus">
      <div className="dash2-gpus__heading">{t('GPUs')}</div>
      <div className="dash2-gpus__list" role="group" aria-label={t('GPU history selection')}>
        {gpus.slice(0, 4).map((gpu, index) => {
          const usage = reading(gpu.gpu_percent);
          const used = reading(gpu.memory_used_gb);
          const total = reading(gpu.memory_total_gb);
          const memoryPct = used != null && total != null && total > 0 ? Math.min(100, used / total * 100) : 0;
          const memoryLabel = t(gpu.memory_type === 'dedicated' ? 'VRAM' : 'MEM');
          const hasTotal = total != null && total > 0;
          const memoryText = used == null ? '—' : `${gb(used)}${hasTotal ? `/${gb(total)}` : ''} GB`;
          const usageText = usage == null ? '—' : `${Math.round(usage)}%`;
          const memoryThreshold = hasTotal ? threshold : null;
          const meters = [
            { kind: 'gpu', label: t('GPU'), text: usageText, width: Math.min(100, usage ?? 0), threshold: null },
            { kind: 'memory', label: memoryLabel, text: memoryText, width: memoryPct, threshold: memoryThreshold },
          ];
          return (
            <button key={gpu.id} type="button" className="dash2-gpus__device"
              aria-pressed={selectedId === gpu.id}
              aria-label={t('GPU {index}, {name}. Utilization {usage}. {memory} {value}. Show history.', {
                index, name: gpu.name, usage: usageText, memory: memoryLabel, value: memoryText,
              })}
              title={`${gpu.name} (${gpu.id})`}
              onClick={() => onSelect(gpu.id)}
            >
              <span className="dash2-gpus__identity">
                <strong>GPU {index}</strong>
                <span className="dash2-gpus__name">{gpu.name}</span>
              </span>
              <span className="dash2-gpus__meters">
                {meters.map(meter => (
                  <span className="dash2-gpus__meter" key={meter.kind}>
                    <span className="dash2-gpus__meter-name">{meter.label}</span>
                    <span className="dash2-gpus__track" aria-hidden="true">
                      <span className={`dash2-gpus__fill dash2-gpus__fill--${meter.kind}${
                        meter.threshold != null && used != null && meter.width >= meter.threshold ? ' dash2-gpus__fill--over' : ''}`}
                        style={{ width: `${meter.width}%` }} />
                      {meter.threshold != null && (
                        <span className="dash2-gpus__threshold" data-eviction-threshold={meter.threshold}
                          title={`Eviction threshold: ${Number(meter.threshold.toFixed(2))}%`}
                          style={{ left: `${meter.threshold}%` }} />
                      )}
                    </span>
                    <span className="dash2-gpus__meter-value">{meter.text}</span>
                  </span>
                ))}
              </span>
            </button>
          );
        })}
      </div>
      {gpus.length > 4 && <span className="dash2-gpus__more">{t('+{count} GPUs', { count: gpus.length - 4 })}</span>}
    </div>
  );
};
