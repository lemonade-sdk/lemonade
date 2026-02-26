import React, { useState, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { serverFetch } from '../../utils/serverConfig';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import TypingIndicator from '../TypingIndicator';

interface RerankingPanelProps {
  isBusy: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
}

const RerankIcon: React.FC = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
    <path
      d="M3 8L6 5L9 8M6 5V19M21 16L18 19L15 16M18 19V5"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
    />
  </svg>
);

const RerankingPanel: React.FC<RerankingPanelProps> = ({
  isBusy, isInferring, activeModality,
  runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();
  const [rerankQuery, setRerankQuery] = useState('');
  const [rerankDocuments, setRerankDocuments] = useState('');
  const [rerankHistory, setRerankHistory] = useState<Array<{
    query: string;
    documents: string;
    results: Array<{ index: number; text: string; score: number }>;
  }>>([]);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  const handleReranking = async () => {
    if (!rerankQuery.trim() || !rerankDocuments.trim() || isBusy) return;

    const ready = await runPreFlight('reranking', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    const currentQuery = rerankQuery;
    const currentDocuments = rerankDocuments;

    try {
      const docs = currentDocuments.split('\n').filter(d => d.trim());

      const response = await serverFetch('/reranking', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model: selectedModel,
          query: currentQuery,
          documents: docs,
        }),
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      const data = await response.json();

      if (data.results && Array.isArray(data.results)) {
        const results = data.results.map((r: any) => ({
          index: r.index,
          text: docs[r.index],
          score: r.relevance_score || r.score || 0,
        }));

        setRerankHistory(prev => [...prev, {
          query: currentQuery,
          documents: currentDocuments,
          results,
        }]);
      } else {
        throw new Error('Unexpected response format');
      }
    } catch (error: any) {
      console.error('Failed to rerank:', error);
      showError(`Failed to rerank: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  return (
    <>
      <div className="chat-messages">
        {rerankHistory.length === 0 && <EmptyState title="Lemonade Reranking" />}

        {rerankHistory.map((item, index) => (
          <div key={index} className="reranking-history-item">
            <div className="reranking-user-input">
              <div className="reranking-input-label">Query</div>
              <div className="reranking-input-text">{item.query}</div>
            </div>
            <div className="reranking-user-input">
              <div className="reranking-input-label">Documents</div>
              <div className="reranking-input-text">{item.documents.split('\n').filter(d => d.trim()).length} documents</div>
            </div>
            <div className="reranking-result-container">
              <div className="reranking-result-header">
                <h4>Ranked Results</h4>
                <span className="reranking-count-badge">{item.results.length} results</span>
              </div>
              <div className="reranking-result">
                {item.results.map((doc, idx) => (
                  <div key={idx} className="reranked-document">
                    <span className="reranked-rank">#{idx + 1}</span>
                    <span className="reranked-score">{doc.score.toFixed(3)}</span>
                    <span className="reranked-document-text">{doc.text}</span>
                  </div>
                ))}
              </div>
            </div>
          </div>
        ))}

        {isBusy && activeModality === 'reranking' && (
          <div className="chat-message assistant-message">
            <TypingIndicator />
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper reranking-input-wrapper">
          <div className="reranking-query-section">
            <label className="reranking-label">Query</label>
            <textarea
              className="chat-input reranking-query"
              value={rerankQuery}
              onChange={(e) => {
                setRerankQuery(e.target.value);
                adjustTextareaHeight(e.target);
              }}
              placeholder="Enter your search query..."
              rows={1}
            />
          </div>
          <div className="reranking-documents-section">
            <label className="reranking-label">Documents (one per line)</label>
            <textarea
              className="chat-input reranking-documents"
              value={rerankDocuments}
              onChange={(e) => {
                setRerankDocuments(e.target.value);
                adjustTextareaHeight(e.target);
              }}
              placeholder="Enter documents to rerank, one per line..."
              rows={3}
            />
          </div>
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleReranking}
            sendDisabled={!rerankQuery.trim() || !rerankDocuments.trim() || isBusy}
            sendIcon={<RerankIcon />}
            sendTitle="Rerank documents"
            modelSelector={<ModelSelector disabled={isBusy} />}
          />
        </div>
      </div>
    </>
  );
};

export default RerankingPanel;
