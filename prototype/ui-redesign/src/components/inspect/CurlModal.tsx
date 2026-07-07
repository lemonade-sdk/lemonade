import React, { useState, useMemo, useEffect, useRef } from 'react';
import api from '../../api';
import { type Trace, inspectStore } from '../../inspectStore';
import { Icon } from '../Icon';
import Modal from './Modal';

interface CurlModalProps {
  isOpen: boolean;
  onClose: () => void;
  selectedTrace: Trace;
  replaySystemPrompt: string;
  replayTemp: number;
  replayTopP: number;
  replayTopK: number;
  replayMaxTokens: number;
  handleCopyFull: (text: string, label: string) => void;
}

export default function CurlModal({
  isOpen,
  onClose,
  selectedTrace,
  replaySystemPrompt,
  replayTemp,
  replayTopP,
  replayTopK,
  replayMaxTokens,
  handleCopyFull
}: CurlModalProps) {
  const [replayProtocol, setReplayProtocol] = useState<'openai' | 'responses' | 'anthropic'>('openai');

  const curlCommand = useMemo(() => {
    if (!selectedTrace) return '';
    const userMsgs = selectedTrace.messages
      .filter((m) => m.role !== 'system')
      .map((m) => ({ role: m.role, content: m.content }));
    const systemContent = replaySystemPrompt.trim();

    let url = `${api.baseUrl}/api/v1/chat/completions`;
    let body: any = {};

    if (replayProtocol === 'openai') {
      const messages = [];
      if (systemContent) {
        messages.push({ role: 'system', content: systemContent });
      }
      messages.push(...userMsgs);
      body = {
        model: selectedTrace.model,
        messages,
        temperature: replayTemp,
        top_p: replayTopP,
        top_k: replayTopK,
        max_tokens: replayMaxTokens,
        stream: false,
      };
    } else if (replayProtocol === 'responses') {
      url = `${api.baseUrl}/api/v1/responses`;
      body = {
        model: selectedTrace.model,
        instructions: systemContent || undefined,
        input: userMsgs.map((m) => m.content),
        temperature: replayTemp,
        top_p: replayTopP,
        max_output_tokens: replayMaxTokens,
        stream: false,
      };
    } else if (replayProtocol === 'anthropic') {
      url = `${api.baseUrl}/v1/messages`;
      body = {
        model: selectedTrace.model,
        system: systemContent || undefined,
        messages: userMsgs,
        temperature: replayTemp,
        top_p: replayTopP,
        top_k: replayTopK,
        max_tokens: replayMaxTokens,
        stream: false,
      };
    }

    const authHeader = api.apiKey ? '  -H "Authorization: Bearer $LEMONADE_API_KEY" \\\n' : '';
    const sessionHeader = `  -H "X-Client-Session-Id: ${api.clientSessionId}" \\\n`;

    // Shell escape any single quotes to prevent injection in curl -d '...'
    const jsonBody = JSON.stringify(body, null, 2);
    const escapedBody = jsonBody.replace(/'/g, "'\\''");

    return `curl ${url} \\\n  -H "Content-Type: application/json" \\\n${authHeader}${sessionHeader}  -d '${escapedBody}'`;
  }, [selectedTrace, replaySystemPrompt, replayTemp, replayTopP, replayTopK, replayMaxTokens, replayProtocol]);

  return (
    <Modal
      isOpen={isOpen}
      onClose={onClose}
      title="cURL Request Preview"
      ariaLabelledBy="curl-modal-title"
      maxWidth="640px"
    >
      <div className="inspect-modal-body flex-col gap-14">
        <p style={{ margin: 0, fontSize: 'var(--text-xs)', color: 'var(--text-secondary)' }}>
          Choose a format and copy the CLI command to execute the same API request against your local server.
        </p>

        {/* Protocol selector & Copy */}
        <div className="flex-row justify-between align-center" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 'var(--space-2)' }}>
          <div className="protocol-selector-segmented">
            <button
              type="button"
              className={replayProtocol === 'openai' ? 'active' : ''}
              onClick={() => setReplayProtocol('openai')}
            >
              Chat Completions
            </button>
            <button
              type="button"
              className={replayProtocol === 'responses' ? 'active' : ''}
              onClick={() => setReplayProtocol('responses')}
            >
              Responses
            </button>
            <button
              type="button"
              className={replayProtocol === 'anthropic' ? 'active' : ''}
              onClick={() => setReplayProtocol('anthropic')}
            >
              Anthropic Messages
            </button>
          </div>
          <button
            type="button"
            className="replay-btn outline"
            onClick={() => handleCopyFull(curlCommand, 'cURL command')}
            style={{ display: 'inline-flex', alignItems: 'center', gap: '6px' }}
          >
            <Icon name="copy" size={14} /> Copy command
          </button>
        </div>

        {/* Box */}
        <div className="curl-box" style={{ height: '260px', overflowY: 'auto', background: 'var(--surface-2)', border: '1px solid var(--border-subtle)', borderRadius: 'var(--radius-md)', padding: 'var(--space-3)' }}>
          <pre style={{ margin: 0, whiteSpace: 'pre-wrap', fontFamily: 'var(--font-mono, monospace)', fontSize: 'var(--text-xs)', color: 'var(--text-primary)' }}>{curlCommand}</pre>
        </div>
      </div>

      <div className="inspect-modal-footer">
        <button
          type="button"
          className="inspect-footer-btn outline"
          onClick={onClose}
        >
          Close
        </button>
      </div>
    </Modal>
  );
}
