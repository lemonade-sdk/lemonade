import React, { useEffect, useState } from 'react';
import { Icon } from './Icon';
import type { DownloadListItem } from '../features/downloadManager/downloadStore';
import { useI18n } from '../i18n';

type PreparationPhase = 'waiting' | 'downloading' | 'loading';

type RelevantDownload = {
  id: string;
  kind: 'backend' | 'model';
  percent: number;
  paused: boolean;
};

interface ChatReadinessProgressProps {
  modelName: string;
  recipe: string;
  modelSizeGiB?: number;
  phase: PreparationPhase | null;
  isLoaded: boolean;
}

/** ModelInfo.size is GiB; the UX estimate assumes 1 decimal GB/s + 1 s startup. */
export function estimatedModelLoadPercent(sizeGiB: number | undefined, elapsedMs: number): number | null {
  if (sizeGiB === undefined || !Number.isFinite(sizeGiB) || sizeGiB <= 0) return null;
  const estimatedMs = 1000 + sizeGiB * (1024 ** 3 / 1_000_000_000) * 1000;
  return Math.max(0, Math.min(99, Math.floor(Math.max(0, elapsedMs) / estimatedMs * 99)));
}

/** Only work that can make the selected chat model unavailable belongs in this indicator. */
export function relevantChatDownload(
  downloads: DownloadListItem[], modelName: string, recipe: string,
): RelevantDownload | null {
  const target = modelName.trim().toLowerCase();
  const backendRecipe = recipe.trim().toLowerCase();
  if (!target) return null;
  const pending = (item: DownloadListItem) => item.running === true
    || item.status === 'downloading' || item.status === 'paused';
  const model = downloads.find(item => item.downloadType === 'model' && pending(item) && (
    item.modelName.toLowerCase() === target
    || item.id.toLowerCase() === `model:${target}`
    || item.id.toLowerCase().endsWith(`:${target}`)
  ));
  const backend = backendRecipe ? downloads.find(item => item.downloadType === 'backend' && pending(item) && (
    item.modelName.toLowerCase().startsWith(`${backendRecipe}:`)
    || item.id.toLowerCase().startsWith(`backend:${backendRecipe}:`)
  )) : undefined;
  const active = model || backend;
  if (!active) return null;
  return {
    id: active.id,
    kind: active.downloadType,
    percent: Math.max(0, Math.min(99, Math.round(Number.isFinite(active.percent) ? active.percent : 0))),
    paused: active.status === 'paused',
  };
}

/** Isolated from ChatView: 1 Hz download updates never rerender the message thread. */
const ChatReadinessProgress: React.FC<ChatReadinessProgressProps> = React.memo(({
  modelName, recipe, modelSizeGiB, phase, isLoaded,
}) => {
  const { t } = useI18n();
  const [download, setDownload] = useState<RelevantDownload | null>(null);
  const [elapsedMs, setElapsedMs] = useState(0);
  const shouldTrack = Boolean(modelName) && (!isLoaded || phase !== null);

  useEffect(() => {
    let cancelled = false;
    let unsubscribe: (() => void) | undefined;
    setDownload(null);
    if (!shouldTrack) return;
    void import(/* webpackChunkName: "download-store" */ '../features/downloadManager/downloadStore')
      .then(({ downloadStore }) => {
        if (cancelled) return;
        unsubscribe = downloadStore.subscribe(items => {
          const next = relevantChatDownload(items, modelName, recipe);
          setDownload(previous => previous?.id === next?.id
            && previous?.kind === next?.kind
            && previous?.percent === next?.percent
            && previous?.paused === next?.paused ? previous : next);
        });
      })
      .catch(error => console.warn('Failed to watch chat preparation downloads:', error));
    return () => { cancelled = true; unsubscribe?.(); };
  }, [modelName, recipe, shouldTrack]);

  // A new loading phase (including the end of a backend install) starts a new estimate.
  const stage = download?.kind || phase;
  useEffect(() => {
    if (stage !== 'loading') return;
    const startedAt = Date.now();
    const tick = () => setElapsedMs(Date.now() - startedAt);
    tick();
    const timer = window.setInterval(tick, 150);
    return () => window.clearInterval(timer);
  }, [modelName, stage]);

  if (!shouldTrack || !stage) return null;
  const label = stage === 'backend' ? t('Installing backend')
    : stage === 'model' || stage === 'downloading' ? t('Downloading model')
    : stage === 'loading' ? t('Loading model') : t('Waiting for model');
  const percent = download ? download.percent
    : stage === 'downloading' ? 0
    : stage === 'loading' ? estimatedModelLoadPercent(modelSizeGiB, elapsedMs) : null;

  return (
    <div className="composer__readiness-anchor">
      <div className="composer__live composer__readiness" title={modelName}>
        <div className="composer__live-head">
          <Icon name="clock" size={14} className="composer__readiness-icon" />
          <span className="composer__readiness-label" role="status" aria-live="polite">
            {download?.paused ? t('Paused') : label}
          </span>
          <span
            className="composer__live-meter"
            role="progressbar"
            aria-label={`${label}: ${modelName}`}
            aria-valuemin={0}
            aria-valuemax={100}
            aria-valuenow={download ? percent ?? undefined : undefined}
            aria-valuetext={label}
          >
            <span className={percent === null ? 'is-indeterminate' : undefined} style={percent !== null ? { width: `${percent}%` } : undefined} />
          </span>
        </div>
      </div>
    </div>
  );
});

export default ChatReadinessProgress;
