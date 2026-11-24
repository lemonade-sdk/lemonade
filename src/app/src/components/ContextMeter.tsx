import React, { useEffect, useMemo, useRef, useState } from 'react';
import api, { type ChatMessage, type LoadedModel, type ModelInfo } from '../api';
import { carryMeasuredImageCost, effectiveSlotContextSize, estimateChatContextUsage, withPendingImageCost, type ChatContextUsage } from '../features/chat/contextUsage';
import { useI18n } from '../i18n';
import './ContextMeter.css';

interface ContextMeterProps {
  model: LoadedModel | null;
  modelInfo?: ModelInfo | null;
  usage: ChatContextUsage | null;
  history?: ChatMessage[];
  previousUsage?: ChatContextUsage | null;
  pendingImageCount?: number;
  mcpEnabled?: boolean;
  mcpServerIds?: string[];
  mcpToolNames?: string[] | null;
}

const CIRCUMFERENCE = 2 * Math.PI * 9;
const format = (count: number) => count.toLocaleString();

type PartId = 'system' | 'definitions' | 'conversation' | 'results' | 'other' | 'thinking' | 'answer';
interface Part { id: PartId; label: string; tokens: number; estimated: boolean }

/** One 30px circular button, directly before Effective Settings. */
const ContextMeter: React.FC<ContextMeterProps> = ({
  model, modelInfo, usage, history = [], previousUsage = null, pendingImageCount = 0,
  mcpEnabled = false, mcpServerIds = [], mcpToolNames = null,
}) => {
  const { t } = useI18n();
  const rootRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const [open, setOpen] = useState(false);
  const [highlighted, setHighlighted] = useState<PartId | null>(null);
  const [windowTokens, setWindowTokens] = useState<number | null>(null);
  const [preflight, setPreflight] = useState<{
    key: string; tools: Record<string, unknown>[]; runtimePrompt: string;
  } | null>(null);

  // Use the effective *runtime* context, not the GGUF's trained maximum.
  useEffect(() => {
    let cancelled = false;
    setWindowTokens(null);
    if (model) {
      void api.loadedModelContextSize(model).then(size => {
        if (!cancelled) setWindowTokens(effectiveSlotContextSize(size, model.recipe, model.launch_command));
      }).catch(() => { if (!cancelled) setWindowTokens(null); });
    }
    return () => { cancelled = true; };
  }, [model?.model_name, model?.pid, model?.recipe_options?.ctx_size, model?.recipe, model?.launch_command?.join(' ')]);

  const systemPrompt = typeof modelInfo?.system_prompt === 'string' ? modelInfo.system_prompt : '';
  const selectionKey = `${mcpEnabled}:${mcpServerIds.join(',')}:${mcpToolNames?.join(',') ?? '*'}`;
  const preflightKey = `${model?.model_name ?? modelInfo?.id ?? ''}:${systemPrompt}:${selectionKey}`;

  // Resolve tool schemas only when the selection changes. Conversation updates
  // and image attachments should not clear the baseline or refetch the runtime.
  useEffect(() => {
    let cancelled = false;
    const publish = (tools: Record<string, unknown>[] = [], runtimePrompt = '') => {
      if (!cancelled) setPreflight({ key: preflightKey, tools, runtimePrompt });
    };
    if (mcpEnabled && mcpServerIds.length) {
      void import('../tools/mcpRuntime')
        .then(({ buildSelectedMcpRuntime }) => buildSelectedMcpRuntime(
          mcpServerIds, {}, mcpToolNames ?? undefined,
        ))
        .then(runtime => publish(runtime?.tools || [], runtime?.systemPrompt || ''))
        .catch(() => publish());
    } else {
      publish();
    }
    return () => { cancelled = true; };
  }, [preflightKey]);

  const baseline = useMemo(() => {
    const active = preflight?.key === preflightKey ? preflight : null;
    const system = [systemPrompt, active?.runtimePrompt].filter(Boolean).join('\n\n');
    return estimateChatContextUsage(
      system ? [{ role: 'system', content: system }, ...history] : history,
      active?.tools || [],
    );
  }, [preflight, preflightKey, systemPrompt, history]);

  useEffect(() => { setOpen(false); setHighlighted(null); }, [model?.model_name]);
  useEffect(() => {
    if (!open) return;
    const onOutside = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const onEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        if (rootRef.current?.contains(document.activeElement)) triggerRef.current?.focus();
        setOpen(false);
      }
    };
    document.addEventListener('pointerdown', onOutside);
    document.addEventListener('keydown', onEscape);
    return () => {
      document.removeEventListener('pointerdown', onOutside);
      document.removeEventListener('keydown', onEscape);
    };
  }, [open]);

  const candidate = usage ?? (baseline.promptTokens > 0 || pendingImageCount > 0 ? baseline : null);
  const displayed = candidate ? withPendingImageCost(
    carryMeasuredImageCost(candidate, previousUsage), pendingImageCount, previousUsage,
  ) : null;
  const instructionsOmitted = !!usage && !usage.promptEstimated
    && ((baseline.systemTokens > 0 && usage.systemTokens === 0)
      || (baseline.toolDefinitionTokens > 0 && usage.toolDefinitionTokens === 0));
  const used = displayed ? displayed.promptTokens + (displayed.completionTokens ?? 0) : null;
  const percent = used !== null && windowTokens !== null && windowTokens > 0
    ? Math.min(100, Math.round((used / windowTokens) * 100)) : null;
  const percentLabel = percent === null ? '—' : percent === 0 && used! > 0 ? '<1%' : `${percent}%`;
  const remaining = used !== null && windowTokens !== null
    ? Math.max(0, windowTokens - used) : null;
  const level = percent === null ? 'unknown' : percent >= 95 ? 'danger' : percent >= 80 ? 'warning' : 'normal';

  const parts = useMemo<Part[]>(() => displayed ? [
    { id: 'system', label: t('System prompt'), tokens: displayed.systemTokens, estimated: true },
    { id: 'definitions', label: t('Tool definitions'), tokens: displayed.toolDefinitionTokens, estimated: true },
    { id: 'conversation', label: t('Conversation text'), tokens: displayed.conversationTokens, estimated: true },
    { id: 'results', label: t('Tool results'), tokens: displayed.toolResultTokens, estimated: true },
    { id: 'other', label: t('Template and media'), tokens: displayed.otherTokens, estimated: true },
    ...(displayed.thinkingTokens ? [{ id: 'thinking' as const, label: t('Thinking text'), tokens: displayed.thinkingTokens, estimated: true }] : []),
    ...(displayed.completionTokens !== null ? [{
      id: 'answer' as const,
      label: t('Last answer'),
      tokens: displayed.answerTokens ?? displayed.completionTokens,
      estimated: displayed.completionEstimated === true || !!displayed.thinkingTokens,
    }] : []),
  ] : [], [displayed, t]);

  // The bar uses the *window* as denominator. A tiny hovered section also gets
  // an outlined marker (minimum 4px), without falsifying the segment widths.
  const segments = useMemo(() => {
    if (!windowTokens) return [];
    let cumulative = 0;
    return parts.filter(part => part.tokens > 0).map(part => {
      const left = Math.min(100, cumulative / windowTokens * 100);
      const width = Math.min(Math.max(0, 100 - left), part.tokens / windowTokens * 100);
      cumulative += part.tokens;
      return { ...part, left, width };
    });
  }, [parts, windowTokens]);
  const activeSegment = segments.find(part => part.id === highlighted);

  return (
    <div className="composer__context-meter" ref={rootRef}>
      <button
        ref={triggerRef}
        type="button"
        className="composer__tools-toggle composer__context-trigger"
        aria-label={`${t('Context usage')}: ${percent === null ? t('Unavailable') : percentLabel}`}
        title={`${t('Context usage')}${percent !== null ? ` · ${percentLabel}` : ''}`}
        aria-haspopup="dialog"
        aria-expanded={open}
        onClick={() => setOpen(value => !value)}
        data-level={level}
      >
        <svg viewBox="0 0 24 24" width="21" height="21" aria-hidden="true">
          <circle cx="12" cy="12" r="9" className="context-meter__track" />
          {percent !== null ? (
            <circle cx="12" cy="12" r="9" className="context-meter__fill"
              transform="rotate(-90 12 12)"
              strokeDasharray={`${CIRCUMFERENCE * percent / 100} ${CIRCUMFERENCE}`} />
          ) : (
            <circle cx="12" cy="12" r="2" className="context-meter__unknown" />
          )}
        </svg>
      </button>
      {open && (
        <section className="context-meter__popover" role="dialog" aria-label={t('Context usage')}>
          <div className="context-meter__heading">
            <strong>{t('Context usage')}</strong>
            <span>{percentLabel}</span>
          </div>
          <div className="context-meter__total">
            <strong>{used === null ? '—' : format(used)}</strong>
            <span> / {windowTokens === null ? '—' : format(windowTokens)} {t('Tokens')}</span>
          </div>
          {displayed && windowTokens !== null && (
            <div className="context-meter__stack" aria-hidden="true">
              <div className="context-meter__stack-segments">
                {segments.map(part => (
                  <span key={part.id} className={`context-meter__part--${part.id}`}
                    data-dimmed={highlighted !== null && highlighted !== part.id}
                    style={{ width: `${part.width}%` }} />
                ))}
              </div>
              {activeSegment && (
                <span className="context-meter__stack-highlight"
                  style={{ left: `${activeSegment.left}%`, width: `${activeSegment.width}%` }} />
              )}
            </div>
          )}
          {displayed ? (
            <>
              <p className="context-meter__caption">{t('Breakdown (estimated)')}</p>
              <dl className="context-meter__rows">
                {parts.map(part => (
                  <div className="context-meter__row" key={part.id} tabIndex={0}
                    data-highlighted={highlighted === part.id}
                    onMouseEnter={() => setHighlighted(part.id)}
                    onMouseLeave={() => setHighlighted(null)}
                    onFocus={() => setHighlighted(part.id)}
                    onBlur={() => setHighlighted(null)}
                    onClick={() => setHighlighted(highlighted === part.id ? null : part.id)}
                    title={windowTokens ? t('Of context window: {value}%', { value: (part.tokens / windowTokens * 100).toFixed(2) }) : undefined}
                  >
                    <dt><span className={`context-meter__dot context-meter__part--${part.id}`} />{part.label}</dt>
                    <dd>{format(part.tokens)}</dd>
                  </div>
                ))}
                <div className="context-meter__row context-meter__row--total">
                  <dt>{t('Prompt total')}</dt><dd>{format(displayed.promptTokens)}</dd>
                </div>
                {displayed.cachedTokens !== null && (
                  <div className="context-meter__row context-meter__row--quiet">
                    <dt>{t('Cached in prompt')}</dt><dd>{format(displayed.cachedTokens)}</dd>
                  </div>
                )}
                {remaining !== null && (
                  <div className="context-meter__row context-meter__row--total">
                    <dt>{t('Available')}</dt><dd>{format(remaining)}</dd>
                  </div>
                )}
              </dl>
              <p className="context-meter__note">
                {displayed.promptEstimated
                  ? (displayed.imageCount
                    ? t('Approximate usage; image tokens vary by model.')
                    : t('Approximate usage; based on available text.'))
                  : t('Backend total measured; breakdown estimated from text.')}
              </p>
              {instructionsOmitted && (
                <p className="context-meter__note">{t('This request may have omitted configured system or tool instructions.')}</p>
              )}
              {!!displayed.thinkingTokens && (
                <p className="context-meter__note">{t('Thinking is included in the output total, not in the next prompt.')}</p>
              )}
              {displayed.completionEstimated && displayed.completionTokens !== null && displayed.completionTokens > 0 && (
                <p className="context-meter__note">{t('Output is estimated while streaming or when backend usage is unavailable.')}</p>
              )}
              {windowTokens !== null && used !== null && used > windowTokens && (
                <p className="context-meter__note context-meter__note--alert">{t('Estimated usage exceeds the context window.')}</p>
              )}
            </>
          ) : (
            <p className="context-meter__note">{t('No token usage reported yet.')}</p>
          )}
          {windowTokens === null && (
            <p className="context-meter__note">{t('Backend context size unavailable.')}</p>
          )}
        </section>
      )}
    </div>
  );
};

export default ContextMeter;
