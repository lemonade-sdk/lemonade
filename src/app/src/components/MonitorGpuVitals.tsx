import React from 'react';
import type { SystemGpuStats } from '../api';
import type { Translate } from '../i18n';

type Props = {
  gpus: SystemGpuStats[];
  selectedId: string | null;
  onSelect: (id: string) => void;
  t: Translate;
};

const reading = (value: number | null): number | null =>
  value != null && Number.isFinite(value) && value >= 0 ? value : null;

export const MonitorGpuVitals: React.FC<Props> = ({ gpus, selectedId, onSelect, t }) => (
  <div className="dash2-gpus">
    <div className="dash2-gpus__heading">{t('GPUs')}</div>
    <div className="dash2-gpus__list" role="group" aria-label={t('GPU history selection')}>
      {gpus.slice(0, 4).map((gpu, index) => {
        const usage = reading(gpu.gpu_percent);
        const used = reading(gpu.memory_used_gb);
        const total = reading(gpu.memory_total_gb);
        const memoryPct = used != null && total != null && total > 0 ? Math.min(100, used / total * 100) : 0;
        const memoryLabel = t(gpu.memory_type === 'dedicated' ? 'VRAM' : 'MEM');
        const memoryText = used == null ? '—' : `${used.toFixed(1)}${total != null && total > 0 ? `/${Number.isInteger(total) ? total : total.toFixed(1)}` : ''} GB`;
        const usageText = usage == null ? '—' : `${Math.round(usage)}%`;
        const meters = [
          { kind: 'gpu', label: t('GPU'), text: usageText, width: Math.min(100, usage ?? 0) },
          { kind: 'memory', label: memoryLabel, text: memoryText, width: memoryPct },
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
                    <span className={`dash2-gpus__fill dash2-gpus__fill--${meter.kind}`}
                      style={{ width: `${meter.width}%` }} />
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
