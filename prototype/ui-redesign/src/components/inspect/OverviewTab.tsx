import React from 'react';
import { type Trace } from '../../inspectStore';
import { Icon } from '../Icon';

interface OverviewTabProps {
  selectedTrace: Trace;
  setActiveTab: (tab: 'overview' | 'messages' | 'replay' | 'improve') => void;
}

export default function OverviewTab({ selectedTrace, setActiveTab }: OverviewTabProps) {
  // Guard duration to avoid NaN% or Infinity% on sub-ms or zero-duration spans
  const safeDur = selectedTrace.dur > 0 ? selectedTrace.dur : 1;
  const queueVal = selectedTrace.queue || 0;
  const prefillVal = selectedTrace.prefill || selectedTrace.ttft || 0;
  const decodeVal = Math.max(0, selectedTrace.dur - queueVal - prefillVal);

  const queueWidth = `${Math.max(2, Math.min(100, (queueVal / safeDur) * 100))}%`;
  const prefillMargin = `${(queueVal / safeDur) * 100}%`;
  const prefillWidth = `${Math.max(2, Math.min(100, (prefillVal / safeDur) * 100))}%`;
  const decodeMargin = `${((queueVal + prefillVal) / safeDur) * 100}%`;
  const decodeWidth = `${Math.max(2, Math.min(100, (decodeVal / safeDur) * 100))}%`;

  return (
    <div id="panel-overview" role="tabpanel" aria-labelledby="tab-overview" className="tab-pane fade-in">
      {selectedTrace.diag && (
        <div className={`health-banner ${selectedTrace.diag.level}`}>
          <span className="health-banner__icon">
            {selectedTrace.diag.level === 'danger' ? <Icon name="x" size={16} /> : <Icon name="alert" size={16} />}
          </span>
          <div className="health-banner__text">
            <strong>{selectedTrace.diag.title}</strong>
            <p>{selectedTrace.diag.detail}</p>
          </div>
          <button className="health-banner__cta" onClick={() => setActiveTab('improve')}>
            Improve <Icon name="chevron-right" size={12} />
          </button>
        </div>
      )}

      {!selectedTrace.diag && (
        <div className="health-banner ok">
          <span className="health-banner__icon">
            <Icon name="check" size={16} />
          </span>
          <div className="health-banner__text">
            <strong>No issues detected</strong>
            <p>TTFT, throughput and context size are within the normal range for this session.</p>
          </div>
          <button className="health-banner__cta" onClick={() => setActiveTab('improve')}>
            Improve <Icon name="chevron-right" size={12} />
          </button>
        </div>
      )}

      {/* Latency Waterfall */}
      <div className="overview-section">
        <h4>SPAN TIMELINE</h4>
        <div className="waterfall-container">
          {/* Queue Segment */}
          <div className="waterfall-row">
            <span className="waterfall-row__label">Queue</span>
            <div className="waterfall-bar-track">
              <div
                className="waterfall-bar queue"
                style={{
                  marginLeft: '0%',
                  width: queueWidth,
                }}
              ></div>
            </div>
            <span className="waterfall-row__val">{selectedTrace.queue ? `${Math.round(selectedTrace.queue)} ms` : '—'}</span>
          </div>

          {/* Prefill/TTFT Segment */}
          <div className="waterfall-row">
            <span className="waterfall-row__label">Prefill (TTFT)</span>
            <div className="waterfall-bar-track">
              <div
                className="waterfall-bar prefill"
                style={{
                  marginLeft: prefillMargin,
                  width: prefillWidth,
                }}
              ></div>
            </div>
            <span className="waterfall-row__val">
              {selectedTrace.prefill || selectedTrace.ttft
                ? `${Math.round(selectedTrace.prefill || selectedTrace.ttft || 0)} ms`
                : '—'}
            </span>
          </div>

          {/* Decode Segment */}
          <div className="waterfall-row">
            <span className="waterfall-row__label">Decode</span>
            <div className="waterfall-bar-track">
              <div
                className="waterfall-bar decode"
                style={{
                  marginLeft: decodeMargin,
                  width: decodeWidth,
                }}
              ></div>
            </div>
            <span className="waterfall-row__val">
              {selectedTrace.prefill || selectedTrace.ttft
                ? `${Math.round(decodeVal)} ms`
                : `${selectedTrace.dur} ms`}
            </span>
          </div>
        </div>
      </div>

      {/* Span Attributes Cards Grid */}
      <div className="overview-section">
        <h4>SPAN ATTRIBUTES</h4>
        <div className="attributes-grid">
          <div className="attribute-card">
            <span className="attribute-card__key">llm.model_name</span>
            <span className="attribute-card__val mono">{selectedTrace.model}</span>
          </div>
          <div className="attribute-card">
            <span className="attribute-card__key">gen_ai.operation.name</span>
            <span className="attribute-card__val mono">{selectedTrace.operation}</span>
          </div>
          <div className="attribute-card">
            <span className="attribute-card__key">openinference.span.kind</span>
            <span className="attribute-card__val mono">{selectedTrace.kind}</span>
          </div>
          <div className="attribute-card">
            <span className="attribute-card__key">gen_ai.provider.name</span>
            <span className="attribute-card__val mono">{selectedTrace.backend || 'lemonade'}</span>
          </div>
          {selectedTrace.sessionId && (
            <div className="attribute-card">
              <span className="attribute-card__key">openinference.session.id</span>
              <span className="attribute-card__val mono">{selectedTrace.sessionId}</span>
            </div>
          )}
          {selectedTrace.userId && (
            <div className="attribute-card">
              <span className="attribute-card__key">openinference.user.id</span>
              <span className="attribute-card__val mono">{selectedTrace.userId}</span>
            </div>
          )}
          <div className="attribute-card">
            <span className="attribute-card__key">semantic.conventions</span>
            <span className="attribute-card__val mono">openinference, otel_genai</span>
          </div>
          {selectedTrace.prompt !== undefined && selectedTrace.completion !== undefined && (
            <div className="attribute-card">
              <span className="attribute-card__key">llm.usage.total_tokens</span>
              <span className="attribute-card__val mono">{selectedTrace.prompt + selectedTrace.completion}</span>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
