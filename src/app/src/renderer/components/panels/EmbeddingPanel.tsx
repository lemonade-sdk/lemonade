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

interface EmbeddingPanelProps {
  isBusy: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
}

const EmbeddingPanel: React.FC<EmbeddingPanelProps> = ({
  isBusy, isInferring, activeModality,
  runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();
  const [embeddingInput, setEmbeddingInput] = useState('');
  const [embeddingHistory, setEmbeddingHistory] = useState<Array<{ input: string; embedding: number[]; dimensions?: number }>>([]);
  const [expandedEmbeddings, setExpandedEmbeddings] = useState<Set<number>>(new Set());
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  const toggleEmbeddingExpansion = (index: number) => {
    setExpandedEmbeddings(prev => {
      const next = new Set(prev);
      if (next.has(index)) next.delete(index);
      else next.add(index);
      return next;
    });
  };

  const handleEmbedding = async () => {
    if (!embeddingInput.trim() || isBusy) return;

    const ready = await runPreFlight('embedding', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    const currentInput = embeddingInput;
    setEmbeddingInput('');

    try {
      const response = await serverFetch('/embeddings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model: selectedModel, input: currentInput }),
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      const data = await response.json();
      let embedding: number[];
      if (data.data && data.data[0] && data.data[0].embedding) {
        embedding = data.data[0].embedding;
      } else if (Array.isArray(data)) {
        embedding = data;
      } else {
        throw new Error('Unexpected response format');
      }

      setEmbeddingHistory(prev => [...prev, { input: currentInput, embedding }]);
    } catch (error: any) {
      console.error('Failed to get embedding:', error);
      showError(`Failed to get embedding: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  return (
    <>
      <div className="chat-messages">
        {embeddingHistory.length === 0 && <EmptyState title="Lemonade Embeddings" />}

        {embeddingHistory.map((item, index) => {
          const isExpanded = expandedEmbeddings.has(index);
          const previewLength = 10;
          const embeddingPreview = item.embedding.slice(0, previewLength);

          return (
            <div key={index} className="embedding-history-item">
              <div className="embedding-user-input">
                <div className="embedding-input-label">Input</div>
                <div className="embedding-input-text">{item.input}</div>
              </div>
              <div className="embedding-result">
                <div className="embedding-result-header">
                  <h4>Embedding Vector</h4>
                  <span className="embedding-dimensions-badge">{item.embedding.length} dimensions</span>
                </div>
                <div className="embedding-vector">
                  {isExpanded ? (
                    <pre>{JSON.stringify(item.embedding, null, 2)}</pre>
                  ) : (
                    <div className="embedding-preview">
                      <pre>[{embeddingPreview.map(v => v.toFixed(6)).join(', ')}, ...]</pre>
                    </div>
                  )}
                </div>
                <button className="embedding-toggle-button" onClick={() => toggleEmbeddingExpansion(index)}>
                  {isExpanded ? 'Show less' : 'Show all'}
                </button>
                <div className="embedding-stats">
                  <span>Min: {Math.min(...item.embedding).toFixed(6)}</span>
                  <span>Max: {Math.max(...item.embedding).toFixed(6)}</span>
                  <span>Mean: {(item.embedding.reduce((a, b) => a + b, 0) / item.embedding.length).toFixed(6)}</span>
                </div>
              </div>
            </div>
          );
        })}

        {isBusy && activeModality === 'embedding' && (
          <div className="chat-message assistant-message">
            <TypingIndicator />
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          <textarea
            ref={inputRef}
            className="chat-input"
            value={embeddingInput}
            onChange={(e) => {
              setEmbeddingInput(e.target.value);
              adjustTextareaHeight(e.target);
            }}
            onKeyPress={(e) => {
              if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                handleEmbedding();
              }
            }}
            placeholder="Enter text to generate embeddings..."
            rows={1}
          />
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleEmbedding}
            sendDisabled={!embeddingInput.trim() || isBusy}
            modelSelector={<ModelSelector disabled={isBusy} />}
          />
        </div>
      </div>
    </>
  );
};

export default EmbeddingPanel;
