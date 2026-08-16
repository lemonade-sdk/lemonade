import type { Trace, TraceDiagnostic } from '../../inspectStore';
import type { TranslationParams } from '../../i18n/types';

type Translate = (key: string, params?: TranslationParams) => string;

const KNOWN_EXECUTION_ERROR_KEYS: Record<string, string> = {
  'client disconnected during stream': 'diagnostics.executionError.clientDisconnected',
};

function localizeExecutionErrorMessage(message: string | undefined, t: Translate): string {
  const raw = String(message || '').trim();
  if (!raw) return t('diagnostics.executionError.unknownDetail');
  const normalized = raw.toLowerCase().replace(/[.!?]+$/g, '').replace(/\s+/g, ' ');
  const key = KNOWN_EXECUTION_ERROR_KEYS[normalized];
  return key ? t(key) : raw;
}

export function traceStatusLabel(status: Trace['status'], t: Translate): string {
  switch (status) {
    case 'slow': return t('traces.slow');
    case 'error': return t('traces.failed');
    default: return t('traces.ok');
  }
}

export function traceDiagnosticText(
  diagnostic: TraceDiagnostic | undefined,
  fallbackDurationMs: number,
  t: Translate,
): { title: string; detail: string } | null {
  if (!diagnostic) return null;

  if (diagnostic.code === 'execution_error') {
    return {
      title: t('diagnostics.executionError.title'),
      detail: localizeExecutionErrorMessage(diagnostic.message, t),
    };
  }

  if (diagnostic.code === 'high_latency') {
    const durationMs = diagnostic.durationMs ?? fallbackDurationMs;
    return {
      title: t('diagnostics.highLatency.title'),
      detail: t('diagnostics.highLatency.detailSeconds', {
        seconds: Math.max(0, Math.round(durationMs / 1000)),
      }),
    };
  }

  // Unknown legacy/custom diagnostics are data supplied by an older build or
  // external source. Preserve them rather than inventing a translation.
  return {
    title: diagnostic.title || t('diagnostics.generic.title'),
    detail: diagnostic.detail || t('diagnostics.generic.detail'),
  };
}
