import React, { useState, useEffect, useRef } from 'react';
import api, { type ChatMessage, type ModelInfo } from '../../api';
import { type Trace, inspectStore } from '../../inspectStore';
import ModelSearchSelector from './ModelSearchSelector';
import { Icon } from '../Icon';
import Modal from './Modal';
import { useAutoScroll } from '../../hooks/useAutoScroll';

interface CreateModalProps {
  isOpen: boolean;
  onClose: () => void;
  availableModels: ModelInfo[];
}

export default function CreateModal({ isOpen, onClose, availableModels }: CreateModalProps) {
  const [modalSystemPrompt, setModalSystemPrompt] = useState('You are a helpful coding assistant. Keep answers brief.');
  const [modalMessages, setModalMessages] = useState<Array<{ role: 'user' | 'assistant'; content: string }>>([
    { role: 'user', content: 'Tell me a joke about compiler optimizations.' }
  ]);
  const [modalSelectedModel, setModalSelectedModel] = useState('');
  const [modalTemp, setModalTemp] = useState(0.7);
  const [modalTopP, setModalTopP] = useState(1.0);
  const [modalTopK, setModalTopK] = useState(50);
  const [modalMaxTokens, setModalMaxTokens] = useState(1024);
  const [modalRunning, setModalRunning] = useState(false);
  const [streamingText, setStreamingText] = useState('');
  const [streamingReasoning, setStreamingReasoning] = useState('');
  const [validationError, setValidationError] = useState<string | null>(null);

  const bodyRef = useRef<HTMLDivElement>(null);
  const outputBoxRef = useRef<HTMLDivElement>(null);

  // Auto-scroll modal body and output box when streaming response updates
  useAutoScroll([bodyRef, outputBoxRef], [streamingText, streamingReasoning], modalRunning);

  // Set default model on open
  useEffect(() => {
    if (isOpen && availableModels.length > 0 && !modalSelectedModel) {
      setModalSelectedModel(availableModels[0].name || availableModels[0].id || '');
    }
  }, [isOpen, availableModels, modalSelectedModel]);

  // Clear validation error when a model is selected
  useEffect(() => {
    if (modalSelectedModel) {
      setValidationError(null);
    }
  }, [modalSelectedModel]);

  const handleCreateRequest = async () => {
    if (!modalSelectedModel) {
      setValidationError('Please select a model');
      inspectStore.showToast('Please select a model');
      return;
    }
    inspectStore.expectIncomingTrace();
    setModalRunning(true);
    setStreamingText('');
    setStreamingReasoning('');

    const requestMessages: ChatMessage[] = [];
    if (modalSystemPrompt.trim()) {
      requestMessages.push({ role: 'system', content: modalSystemPrompt });
    }
    requestMessages.push(...modalMessages.map(m => ({ role: m.role as any, content: m.content })));

    let finalOutput = '';
    let finalReasoning = '';

    try {
      await api.chatCompletion(
        modalSelectedModel,
        requestMessages,
        {
          params: {
            temperature: modalTemp,
            top_p: modalTopP,
            top_k: modalTopK,
            max_tokens: modalMaxTokens,
          },
          onToken: (text) => {
            finalOutput += text;
            setStreamingText(finalOutput);
          },
          onReasoning: (reasoning) => {
            finalReasoning += reasoning;
            setStreamingReasoning(finalReasoning);
          },
          onDone: (stats) => {
            inspectStore.showToast('Completions request succeeded');
            setModalRunning(false);
            onClose();
          },
          onError: (err) => {
            console.error(err);
            inspectStore.cancelExpectIncomingTrace();
            inspectStore.showToast(`Request failed: ${err.message || err}`);
            setModalRunning(false);
          }
        }
      );
    } catch (err: any) {
      console.error(err);
      inspectStore.cancelExpectIncomingTrace();
      inspectStore.showToast(`Request failed: ${err.message || err}`);
      setModalRunning(false);
    }
  };

  return (
    <Modal
      isOpen={isOpen}
      onClose={onClose}
      title="Create Request"
      ariaLabelledBy="create-modal-title"
      maxWidth="600px"
    >
      <div ref={bodyRef} className="inspect-modal-body flex-col gap-14" style={{ height: '450px', maxHeight: '450px', overflowY: 'auto' }}>
        {modalRunning ? (
          <div className="flex-col gap-12" style={{ padding: 'var(--space-2)' }}>
            <div className="flex-row justify-between align-center" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
              <span className="input-label" style={{ color: 'var(--accent)' }}>Streaming live response...</span>
              <span className="replay-loading" style={{ fontSize: 'var(--text-xs)' }}>
                Generating tokens...
              </span>
            </div>

            {streamingReasoning && (
              <div className="reasoning-block" style={{ marginBottom: 'var(--space-2)' }}>
                <div className="reasoning-block__header">
                  <span>Reasoning Process</span>
                </div>
                <div className="reasoning-block__body" style={{ fontStyle: 'italic', opacity: 0.8 }}>
                  {streamingReasoning}
                </div>
              </div>
            )}

            <div ref={outputBoxRef} className="comparison-output-box streaming">
              {streamingText || <span style={{ opacity: 0.5 }}>Waiting for first token...</span>}
              <span className="cursor-blink">|</span>
            </div>
          </div>
        ) : (
          <>
            <ModelSearchSelector
              label="Model"
              value={modalSelectedModel}
              onChange={setModalSelectedModel}
              availableModels={availableModels}
            />

            {!availableModels || availableModels.length === 0 ? (
              <div style={{ color: 'var(--danger)', fontSize: 'var(--text-xs)', marginTop: '-8px' }}>
                No models available. Please pull or install a model first.
              </div>
            ) : validationError ? (
              <div style={{ color: 'var(--danger)', fontSize: 'var(--text-xs)', marginTop: '-8px' }}>
                {validationError}
              </div>
            ) : null}

            {/* System Prompt */}
            <div className="flex-col gap-4">
              <label className="input-label" htmlFor="modal-system-prompt">System prompt</label>
              <textarea
                id="modal-system-prompt"
                value={modalSystemPrompt}
                onChange={(e) => setModalSystemPrompt(e.target.value)}
                placeholder="Define system instructions..."
                rows={2}
                className="system-prompt-textarea"
              />
            </div>

            {/* Messages Manager */}
            <div className="flex-col gap-6">
              <div className="flex-row justify-between align-center" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                <span className="input-label">Messages</span>
                <button
                  type="button"
                  className="add-msg-btn"
                  onClick={() => setModalMessages([...modalMessages, { role: 'user', content: '' }])}
                >
                  + Add Message
                </button>
              </div>

              <div className="modal-messages-list flex-col gap-8">
                {modalMessages.map((msg, index) => (
                  <div key={index} className="modal-message-row" style={{ display: 'flex', gap: 'var(--space-2)', marginBottom: 'var(--space-2)' }}>
                    <select
                      aria-label={`Message ${index + 1} role`}
                      value={msg.role}
                      onChange={(e) => {
                        const updated = [...modalMessages];
                        updated[index].role = e.target.value as 'user' | 'assistant';
                        setModalMessages(updated);
                      }}
                      className="message-role-select"
                    >
                      <option value="user">User</option>
                      <option value="assistant">Assistant</option>
                    </select>
                    <textarea
                      aria-label={`Message ${index + 1} content`}
                      value={msg.content}
                      onChange={(e) => {
                        const updated = [...modalMessages];
                        updated[index].content = e.target.value;
                        setModalMessages(updated);
                      }}
                      placeholder="Type message content..."
                      rows={2}
                      className="message-content-textarea"
                      style={{ flex: 1 }}
                    />
                    <button
                      type="button"
                      className="delete-msg-btn"
                      onClick={() => {
                        const updated = modalMessages.filter((_, i) => i !== index);
                        setModalMessages(updated);
                      }}
                      aria-label={`Delete message ${index + 1}`}
                    >
                      <Icon name="x" size={14} />
                    </button>
                  </div>
                ))}
              </div>
            </div>

            {/* Advanced Parameters */}
            <div className="parameter-grid">
              <div className="slider-row parameter-grid__item">
                <div className="slider-label-row">
                  <label htmlFor="modal-temp">Temperature</label>
                  <span className="val-display">{modalTemp.toFixed(2)}</span>
                </div>
                <input
                  id="modal-temp"
                  type="range"
                  min="0"
                  max="2"
                  step="0.05"
                  value={modalTemp}
                  onChange={(e) => setModalTemp(parseFloat(e.target.value))}
                />
              </div>
              <div className="slider-row parameter-grid__item">
                <div className="slider-label-row">
                  <label htmlFor="modal-max-tokens">Max tokens</label>
                  <span className="val-display">{modalMaxTokens}</span>
                </div>
                <input
                  id="modal-max-tokens"
                  type="range"
                  min="64"
                  max="4096"
                  step="64"
                  value={modalMaxTokens}
                  onChange={(e) => setModalMaxTokens(parseInt(e.target.value, 10))}
                />
              </div>
            </div>
          </>
        )}
      </div>

      <div className="inspect-modal-footer">
        <button
          type="button"
          className="inspect-footer-btn outline"
          onClick={onClose}
          disabled={modalRunning}
        >
          Cancel
        </button>
        <button
          type="button"
          className="inspect-footer-btn primary-simulate"
          onClick={handleCreateRequest}
          disabled={modalRunning || modalMessages.length === 0}
        >
          {modalRunning ? 'Streaming...' : (
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: 'var(--space-1.5)' }}>
              <Icon name="omni" size={14} /> Create request
            </span>
          )}
        </button>
      </div>
    </Modal>
  );
}
