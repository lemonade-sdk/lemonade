import React, { useState, useEffect } from 'react';
import api, { type ChatMessage } from '../../api';
import { type Trace } from '../../inspectStore';
import { Icon } from '../Icon';

interface ReplayTabProps {
  selectedTrace: Trace;
  setCurlModalOpen: (open: boolean) => void;
  // We expose state parameters to let CurlModal read the modified values
  replaySystemPrompt: string;
  setReplaySystemPrompt: (val: string) => void;
  replayTemp: number;
  setReplayTemp: (val: number) => void;
  replayTopP: number;
  setReplayTopP: (val: number) => void;
  replayTopK: number;
  setReplayTopK: (val: number) => void;
  replayMaxTokens: number;
  setReplayMaxTokens: (val: number) => void;
}

export default function ReplayTab({
  selectedTrace,
  setCurlModalOpen,
  replaySystemPrompt,
  setReplaySystemPrompt,
  replayTemp,
  setReplayTemp,
  replayTopP,
  setReplayTopP,
  replayTopK,
  setReplayTopK,
  replayMaxTokens,
  setReplayMaxTokens
}: ReplayTabProps) {
  const [replayOutput, setReplayOutput] = useState('');
  const [replayStats, setReplayStats] = useState<{ ttft: number | null; tps: number | null } | null>(null);
  const [replayRunning, setReplayRunning] = useState(false);

  // Reset local output/status when trace ID changes
  useEffect(() => {
    setReplayOutput('');
    setReplayStats(null);
    setReplayRunning(false);
  }, [selectedTrace.id]);

  const handleRunReplay = async () => {
    if (replayRunning) return;
    setReplayRunning(true);
    setReplayOutput('');
    setReplayStats(null);

    // Reconstruct conversation messages omitting system prompt which we override
    const userMsgs: ChatMessage[] = selectedTrace.messages
      .filter((m) => m.role !== 'system')
      .map((m) => ({ role: m.role as any, content: m.content }));

    const formattedMessages: ChatMessage[] = [];
    if (replaySystemPrompt.trim()) {
      formattedMessages.push({ role: 'system', content: replaySystemPrompt });
    }
    formattedMessages.push(...userMsgs);

    try {
      await api.chatCompletion(selectedTrace.model, formattedMessages, {
        params: {
          temperature: replayTemp,
          top_p: replayTopP,
          top_k: replayTopK,
          max_tokens: replayMaxTokens,
        },
        onToken: (tok) => {
          setReplayOutput((prev) => prev + tok);
        },
        onDone: (stats) => {
          setReplayStats({
            ttft: stats.ttft ? parseInt(stats.ttft, 10) : null,
            tps: stats.tps ? parseFloat(stats.tps) : null,
          });
        },
        onError: (err) => {
          setReplayOutput((prev) => prev + `\n\n[Replay Error: ${err}]`);
        }
      });
    } catch (err: any) {
      setReplayOutput((prev) => prev + `\n\n[Replay Error: ${err.message || err}]`);
    } finally {
      setReplayRunning(false);
    }
  };

  return (
    <div id="panel-replay" role="tabpanel" aria-labelledby="tab-replay" className="tab-pane fade-in flex-col gap-16">
      {/* Parameter adjustment panel */}
      <div className="replay-header-params">
        {/* System prompt box */}
        <div className="replay-prompt-box flex-col gap-4">
          <label className="input-label" htmlFor="replay-system-prompt">System prompt</label>
          <textarea
            id="replay-system-prompt"
            value={replaySystemPrompt}
            onChange={(e) => setReplaySystemPrompt(e.target.value)}
            placeholder="Define system prompt..."
            rows={5}
            className="system-prompt-textarea"
          />
        </div>

        {/* Sliders compact grid */}
        <div className="replay-sliders-grid">
          <div className="slider-row-compact">
            <div className="slider-label-row">
              <label htmlFor="replay-temp">Temperature</label>
              <span className="val-display">{replayTemp.toFixed(2)}</span>
            </div>
            <input
              id="replay-temp"
              type="range"
              min="0"
              max="2"
              step="0.05"
              value={replayTemp}
              onChange={(e) => setReplayTemp(parseFloat(e.target.value))}
            />
          </div>

          <div className="slider-row-compact">
            <div className="slider-label-row">
              <label htmlFor="replay-topp">Top-P</label>
              <span className="val-display">{replayTopP.toFixed(2)}</span>
            </div>
            <input
              id="replay-topp"
              type="range"
              min="0"
              max="1"
              step="0.01"
              value={replayTopP}
              onChange={(e) => setReplayTopP(parseFloat(e.target.value))}
            />
          </div>

          <div className="slider-row-compact">
            <div className="slider-label-row">
              <label htmlFor="replay-topk">Top-K</label>
              <span className="val-display">{replayTopK}</span>
            </div>
            <input
              id="replay-topk"
              type="range"
              min="0"
              max="100"
              step="1"
              value={replayTopK}
              onChange={(e) => setReplayTopK(parseInt(e.target.value, 10))}
            />
          </div>

          <div className="slider-row-compact">
            <div className="slider-label-row">
              <label htmlFor="replay-max-tokens">Max tokens</label>
              <span className="val-display">{replayMaxTokens}</span>
            </div>
            <input
              id="replay-max-tokens"
              type="range"
              min="64"
              max="4096"
              step="64"
              value={replayMaxTokens}
              onChange={(e) => setReplayMaxTokens(parseInt(e.target.value, 10))}
            />
          </div>
        </div>

        {/* Actions column */}
        <div className="replay-actions-column">
          <button
            type="button"
            className="replay-btn primary"
            disabled={replayRunning}
            onClick={handleRunReplay}
            style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center', gap: '6px' }}
          >
            <Icon name="play" size={14} /> Replay
          </button>
          <div className="replay-actions-subrow">
            <button
              type="button"
              className="replay-btn outline"
              disabled={replayRunning}
              onClick={() => {
                const sysMsg = selectedTrace.messages.find((m) => m.role === 'system');
                setReplaySystemPrompt(sysMsg ? sysMsg.content : '');
                setReplayTemp(selectedTrace.temp ?? 0.7);
                setReplayTopP(selectedTrace.topP ?? 1.0);
                setReplayTopK(selectedTrace.topK ?? 50);
                setReplayMaxTokens(selectedTrace.max ?? 1024);
                setReplayOutput('');
                setReplayStats(null);
              }}
            >
              Reset
            </button>
            <button
              type="button"
              className="replay-btn outline"
              onClick={() => setCurlModalOpen(true)}
              title="View local cURL request command"
            >
              cURL
            </button>
          </div>
        </div>
      </div>

      {/* Comparison Area */}
      <div className="replay-results-section">
        <div className="comparison-columns">
          {/* Column 1: Original */}
          <div className="comparison-col">
            <div className="comparison-col__header">
              <h5>ORIGINAL</h5>
              <div className="metric-ministrip">
                <span className="metric-badge">
                  {selectedTrace.ttft ? (
                    <>
                      {Math.round(selectedTrace.ttft)}
                      <span className="metric-card__unit"> ms TTFT</span>
                    </>
                  ) : '—'}
                </span>
                <span className="metric-badge">
                  {selectedTrace.tps ? (
                    <>
                      {Number(selectedTrace.tps).toFixed(1)}
                      <span className="metric-card__unit"> tok/s</span>
                    </>
                  ) : '—'}
                </span>
                <span className="metric-badge">
                  {selectedTrace.completion !== undefined ? (
                    <>
                      {selectedTrace.completion}
                      <span className="metric-card__unit"> tok</span>
                    </>
                  ) : '—'}
                </span>
              </div>
            </div>
            <div className="comparison-output-box">
              {selectedTrace.output}
            </div>
          </div>

          {/* Column 2: Replay */}
          <div className="comparison-col">
            <div className="comparison-col__header">
              <h5>REPLAY</h5>
              {replayStats && (
                <div className="metric-ministrip">
                  <span className="metric-badge">
                    {replayStats.ttft ? (
                      <>
                        {replayStats.ttft}
                        <span className="metric-card__unit"> ms TTFT</span>
                      </>
                    ) : '—'}
                    {selectedTrace.ttft && replayStats.ttft !== null && (
                      <span
                        className={`delta-chip ${replayStats.ttft < selectedTrace.ttft ? 'better' : 'worse'}`}
                        aria-label={`${Math.abs(replayStats.ttft - selectedTrace.ttft)}ms ${replayStats.ttft < selectedTrace.ttft ? 'faster' : 'slower'}`}
                      >
                        {replayStats.ttft < selectedTrace.ttft ? '▼' : '▲'} {Math.abs(replayStats.ttft - selectedTrace.ttft)}ms
                      </span>
                    )}
                  </span>
                  <span className="metric-badge">
                    {replayStats.tps ? (
                      <>
                        {Number(replayStats.tps).toFixed(1)}
                        <span className="metric-card__unit"> tok/s</span>
                      </>
                    ) : '—'}
                    {selectedTrace.tps && replayStats.tps !== null && (
                      <span
                        className={`delta-chip ${Number(replayStats.tps) > Number(selectedTrace.tps) ? 'better' : 'worse'}`}
                        aria-label={`${Math.abs(Number(replayStats.tps) - Number(selectedTrace.tps)).toFixed(1)} tokens per second ${Number(replayStats.tps) > Number(selectedTrace.tps) ? 'faster' : 'slower'}`}
                      >
                        {Number(replayStats.tps) > Number(selectedTrace.tps) ? '▲' : '▼'} {Math.abs(Number(replayStats.tps) - Number(selectedTrace.tps)).toFixed(1)}
                        <span className="metric-card__unit"> tok/s</span>
                      </span>
                    )}
                  </span>
                </div>
              )}
            </div>
            <div className="comparison-output-box replay">
              {replayOutput ? (
                <>
                  {replayOutput}
                  {replayRunning && <span className="streaming-cursor">▋</span>}
                </>
              ) : replayRunning ? (
                <div className="replay-loading">
                  <span className="spinner"></span>
                  Generating output...
                </div>
              ) : (
                <div className="replay-empty-state">
                  <span className="replay-empty-state__icon" style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }}>
                    <Icon name="rotate-ccw" size={24} />
                  </span>
                  <strong className="replay-empty-state__title">Adjust parameters and run</strong>
                  <p className="replay-empty-state__desc">
                    The replay re-runs this exact prompt locally and diffs the metrics against the original.
                  </p>
                </div>
              )}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
