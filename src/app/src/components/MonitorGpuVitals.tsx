import React from 'react';
import type { SystemGpuStats } from '../api';

type Props = {
  gpus: SystemGpuStats[];
  selectedId: string | null;
  onSelect: (id: string) => void;
};

function reading(value: number | null): number | null {
  return value != null && Number.isFinite(value) && value >= 0 ? value : null;
}

function gb(value: number): string {
  return Number.isInteger(value) ? String(value) : value.toFixed(1);
}

export const MonitorGpuVitals: React.FC<Props> = ({ gpus, selectedId, onSelect }) => {
  const visible = gpus.slice(0, 4);
  return (
    <div className="dash2-gpus">
      <div className="dash2-gpus__heading">
        <span>GPUs</span>
      </div>
      <div className="dash2-gpus__list" role="group" aria-label="GPU history selection">
        {visible.map((gpu, index) => {
          const usage = reading(gpu.gpu_percent);
          const used = reading(gpu.memory_used_gb);
          const total = reading(gpu.memory_total_gb);
          const memoryPct = used != null && total != null && total > 0
            ? Math.min(100, used / total * 100)
            : null;
          const memoryLabel = gpu.memory_type === 'shared' ? 'MEM' : 'VRAM';
          const memoryText = used == null ? '—' : `${used.toFixed(1)}${total != null && total > 0 ? `/${gb(total)}` : ''} GB`;
          const usageText = usage == null ? '—' : `${Math.round(usage)}%`;
          return (
            <button
              key={gpu.id}
              type="button"
              className="dash2-gpus__device"
              aria-pressed={selectedId === gpu.id}
              aria-label={`GPU ${index}, ${gpu.name}. Utilization ${usageText}. ${memoryLabel} ${memoryText}. Show history.`}
              title={`${gpu.name} (${gpu.id}) · ${gpu.memory_type} memory`}
              onClick={() => onSelect(gpu.id)}
            >
              <span className="dash2-gpus__identity">
                <strong>GPU {index}</strong>
                <span className="dash2-gpus__name">{gpu.name}</span>
              </span>
              <span className="dash2-gpus__meters">
                <span className="dash2-gpus__meter">
                  <span className="dash2-gpus__meter-name">GPU</span>
                  <span className="dash2-gpus__track" aria-hidden="true">
                    <span className="dash2-gpus__fill dash2-gpus__fill--gpu"
                      style={{ width: `${usage == null ? 0 : Math.min(100, usage)}%` }} />
                  </span>
                  <span className="dash2-gpus__meter-value">{usageText}</span>
                </span>
                <span className="dash2-gpus__meter">
                  <span className="dash2-gpus__meter-name">{memoryLabel}</span>
                  <span className="dash2-gpus__track" aria-hidden="true">
                    <span className="dash2-gpus__fill dash2-gpus__fill--memory"
                      style={{ width: `${memoryPct ?? 0}%` }} />
                  </span>
                  <span className="dash2-gpus__meter-value">{memoryText}</span>
                </span>
              </span>
            </button>
          );
        })}
      </div>
      {gpus.length > visible.length && (
        <span className="dash2-gpus__more">+{gpus.length - visible.length} GPUs</span>
      )}
    </div>
  );
};
