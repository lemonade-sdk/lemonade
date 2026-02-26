import React, { useState, useEffect, useRef, useMemo } from 'react';
import MarkdownMessage from '../../MarkdownMessage';
import AudioButton from '../../AudioButton';
import {
  AppSettings,
  buildChatRequestOverrides,
} from '../../utils/appSettings';
import { serverFetch } from '../../utils/serverConfig';
import { useModels } from '../../hooks/useModels';
import { useSystem } from '../../hooks/useSystem';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { useTTS } from '../../hooks/useTTS';
import { Message, MessageContent, TextContent, ImageContent } from '../../utils/chatTypes';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import { SendIcon, ImageUploadIcon } from '../Icons';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import ImagePreviewList from '../ImagePreviewList';
import EmptyState from '../EmptyState';
import TypingIndicator from '../TypingIndicator';

interface LLMChatPanelProps {
  isBusy: boolean;
  isPreFlight: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
  appSettings: AppSettings | null;
  isVision: boolean;
  currentLoadedModel: string | null;
  setCurrentLoadedModel: React.Dispatch<React.SetStateAction<string | null>>;
}

const LLMChatPanel: React.FC<LLMChatPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality,
  runPreFlight, reset, showError, appSettings,
  isVision, currentLoadedModel, setCurrentLoadedModel,
}) => {
  const { selectedModel, modelsData } = useModels();
  const { systemInfo } = useSystem();
  const tts = useTTS(appSettings, modelsData);

  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [editingImages, setEditingImages] = useState<string[]>([]);
  const [uploadedImages, setUploadedImages] = useState<string[]>([]);
  const [expandedThinking, setExpandedThinking] = useState<Set<number>>(new Set());
  const [isUserAtBottom, setIsUserAtBottom] = useState(true);
  const userScrolledAwayRef = useRef(false);

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const editFileInputRef = useRef<HTMLInputElement>(null);
  const abortControllerRef = useRef<AbortController | null>(null);
  const scrollTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  const autoScrollInProgressRef = useRef(false);
  const autoScrollResetRef = useRef<number | null>(null);
  const autoScrollRafRef = useRef<number | null>(null);
  const pendingAutoScrollRef = useRef(false);

  // Consolidated image handlers
  const createImageHandlers = (
    setImages: React.Dispatch<React.SetStateAction<string[]>>,
    visionGate: boolean,
  ) => ({
    upload: (event: React.ChangeEvent<HTMLInputElement>) => {
      const files = event.target.files;
      if (!files || files.length === 0) return;
      const file = files[0];
      const reader = new FileReader();
      reader.onload = (e) => {
        const result = e.target?.result;
        if (typeof result === 'string') setImages(prev => [...prev, result]);
      };
      reader.readAsDataURL(file);
      event.target.value = '';
    },
    paste: (event: React.ClipboardEvent) => {
      const items = event.clipboardData.items;
      for (let i = 0; i < items.length; i++) {
        const item = items[i];
        if (item.type.indexOf('image') !== -1) {
          event.preventDefault();
          if (visionGate && !isVision) break;
          const file = item.getAsFile();
          if (!file) continue;
          const reader = new FileReader();
          reader.onload = (e) => {
            const result = e.target?.result;
            if (typeof result === 'string') setImages(prev => [...prev, result]);
          };
          reader.readAsDataURL(file);
          break;
        }
      }
    },
    remove: (index: number) => {
      setImages(prev => prev.filter((_, i) => i !== index));
    },
  });

  const uploadedImageHandlers = createImageHandlers(setUploadedImages, true);
  const editingImageHandlers = createImageHandlers(setEditingImages, false);

  // Abort on unmount
  useEffect(() => {
    return () => {
      abortControllerRef.current?.abort();
    };
  }, []);

  // Auto-scroll
  useEffect(() => {
    if (!userScrolledAwayRef.current && isUserAtBottom) {
      if (scrollTimeoutRef.current) clearTimeout(scrollTimeoutRef.current);
      scrollToBottom();
    }
    return () => {
      if (scrollTimeoutRef.current) clearTimeout(scrollTimeoutRef.current);
      if (autoScrollResetRef.current !== null) window.clearTimeout(autoScrollResetRef.current);
      if (autoScrollRafRef.current !== null) {
        window.cancelAnimationFrame(autoScrollRafRef.current);
        autoScrollRafRef.current = null;
      }
      pendingAutoScrollRef.current = false;
    };
  }, [messages, isBusy, isUserAtBottom]);

  // Auto-grow edit textarea
  useEffect(() => {
    if (editTextareaRef.current) {
      editTextareaRef.current.style.height = 'auto';
      editTextareaRef.current.style.height = editTextareaRef.current.scrollHeight + 'px';
    }
  }, [editingIndex, editingValue]);

  // Reset textarea height when input is cleared
  useEffect(() => {
    if (inputTextareaRef.current && inputValue === '') {
      inputTextareaRef.current.style.height = 'auto';
      inputTextareaRef.current.style.overflowY = 'hidden';
    }
  }, [inputValue]);

  const checkIfAtBottom = () => {
    const container = messagesContainerRef.current;
    if (!container) return true;
    const threshold = 20;
    return container.scrollHeight - container.scrollTop - container.clientHeight < threshold;
  };

  const handleScroll = () => {
    const atBottom = checkIfAtBottom();
    setIsUserAtBottom(atBottom);
    if (!atBottom && isInferring && !autoScrollInProgressRef.current) {
      userScrolledAwayRef.current = true;
    } else if (atBottom) {
      userScrolledAwayRef.current = false;
    }
  };

  const scrollToBottom = () => {
    if (pendingAutoScrollRef.current) return;
    pendingAutoScrollRef.current = true;
    if (autoScrollRafRef.current !== null) return;

    autoScrollRafRef.current = window.requestAnimationFrame(() => {
      autoScrollRafRef.current = null;
      pendingAutoScrollRef.current = false;
      if (userScrolledAwayRef.current) return;
      const container = messagesContainerRef.current;
      if (!container) return;
      autoScrollInProgressRef.current = true;
      if (autoScrollResetRef.current !== null) window.clearTimeout(autoScrollResetRef.current);
      container.scrollTop = container.scrollHeight;
      autoScrollResetRef.current = window.setTimeout(() => {
        autoScrollInProgressRef.current = false;
        autoScrollResetRef.current = null;
      }, 60);
    });
  };

  const buildChatRequestBody = (messageHistory: Message[]) => ({
    model: selectedModel,
    messages: messageHistory,
    stream: true,
    ...buildChatRequestOverrides(appSettings),
  });

  /** Build an error message enriched with backend action help text when available. */
  const buildErrorMessage = (error: any): string => {
    const errorMessage = error.message || 'Failed to get response from the model.';
    const modelInfo = modelsData[selectedModel];
    const recipe = modelInfo?.recipe;
    const backendAction = recipe && systemInfo?.recipes?.[recipe]?.backends?.[systemInfo.recipes[recipe].default_backend || '']?.action;
    const helpText = backendAction ? `\n\n${backendAction}` : '';

    if (backendAction && backendAction.match(/https?:\/\/[^\s]+\.html/)) {
      const urlMatch = backendAction.match(/https?:\/\/[^\s]+/);
      if (urlMatch) {
        window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url: urlMatch[0] } }));
      }
    }

    return `Error: ${errorMessage}${helpText}`;
  };

  const extractThinking = (content: string): { content: string; thinking: string } => {
    let extractedThinking = '';
    let cleanedContent = content;
    const thinkRegex = /<think>([\s\S]*?)<\/think>/g;
    let match;
    while ((match = thinkRegex.exec(content)) !== null) {
      extractedThinking += match[1];
    }
    cleanedContent = cleanedContent.replace(thinkRegex, '');
    return { content: cleanedContent, thinking: extractedThinking };
  };

  const handleStreamingResponse = async (messageHistory: Message[]): Promise<void> => {
    let accumulatedContent = '';
    let accumulatedThinking = '';
    let receivedFirstChunk = false;
    let lastRenderUpdateAt = 0;
    let thinkingAutoExpanded = false;
    const STREAM_UPDATE_INTERVAL_MS = 33;
    const isNewModelLoad = currentLoadedModel !== selectedModel;

    const flushAssistantUpdate = (force = false) => {
      const now = Date.now();
      if (!force && now - lastRenderUpdateAt < STREAM_UPDATE_INTERVAL_MS) return;
      lastRenderUpdateAt = now;

      const extracted = extractThinking(accumulatedContent);
      const displayContent = extracted.content;
      const embeddedThinking = extracted.thinking;
      const totalThinking = (accumulatedThinking || '') + (embeddedThinking || '');

      setMessages(prev => {
        if (prev.length === 0) return prev;
        const newMessages = [...prev];
        const messageIndex = newMessages.length - 1;
        newMessages[messageIndex] = {
          role: 'assistant',
          content: displayContent,
          thinking: totalThinking || undefined,
        };
        return newMessages;
      });

      if (totalThinking && !thinkingAutoExpanded && !appSettings?.collapseThinkingByDefault?.value) {
        thinkingAutoExpanded = true;
        setExpandedThinking(prevExpanded => {
          const next = new Set(prevExpanded);
          next.add(messageHistory.length);
          return next;
        });
      }
    };

    const response = await serverFetch('/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(buildChatRequestBody(messageHistory)),
      signal: abortControllerRef.current!.signal,
    });

    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    if (!response.body) throw new Error('Response body is null');

    const reader = response.body.getReader();
    const decoder = new TextDecoder();

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        const chunk = decoder.decode(value, { stream: true });
        const lines = chunk.split('\n');

        for (const line of lines) {
          if (line.startsWith('data: ')) {
            const data = line.slice(6).trim();
            if (data === '[DONE]' || !data) continue;

            try {
              const parsed = JSON.parse(data);
              const delta = parsed.choices?.[0]?.delta;
              const content = delta?.content;
              const thinkingContent = delta?.reasoning_content || delta?.thinking;

              if (content) accumulatedContent += content;
              if (thinkingContent) accumulatedThinking += thinkingContent;

              if (content || thinkingContent) {
                if (!receivedFirstChunk) {
                  receivedFirstChunk = true;
                  setCurrentLoadedModel(selectedModel);
                  if (isNewModelLoad) {
                    window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: selectedModel } }));
                  }
                }
                flushAssistantUpdate();
              }
            } catch (e) {
              console.warn('Failed to parse SSE data:', data, e);
            }
          }
        }
      }
    } finally {
      flushAssistantUpdate(true);
      reader.releaseLock();
    }

    if (!accumulatedContent) throw new Error('No content received from stream');
  };

  const sendMessage = async () => {
    if ((!inputValue.trim() && uploadedImages.length === 0) || isBusy) return;

    const ready = await runPreFlight('llm', {
      modelName: selectedModel,
      modelsData,
      onError: (msg) => {
        setMessages(prev => [...prev, { role: 'assistant', content: `Error preparing model: ${msg}` }]);
      },
    });
    if (!ready) return;

    if (abortControllerRef.current) abortControllerRef.current.abort();
    abortControllerRef.current = new AbortController();
    setIsUserAtBottom(true);
    userScrolledAwayRef.current = false;

    let messageContent: MessageContent;
    if (uploadedImages.length > 0) {
      const contentArray: Array<TextContent | ImageContent> = [];
      if (inputValue.trim()) contentArray.push({ type: 'text', text: inputValue });
      uploadedImages.forEach(imageUrl => {
        contentArray.push({ type: 'image_url', image_url: { url: imageUrl } });
      });
      messageContent = contentArray;
    } else {
      messageContent = inputValue;
    }

    const userMessage: Message = { role: 'user', content: messageContent };
    const messageHistory = [...messages, userMessage];

    setMessages(prev => [...prev, userMessage]);
    setInputValue('');
    setUploadedImages([]);
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    try {
      await handleStreamingResponse(messageHistory);
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        setMessages(prev => {
          const lastMessage = prev[prev.length - 1];
          if (!lastMessage || (!lastMessage.content && !lastMessage.thinking)) return prev.slice(0, -1);
          return prev;
        });
      } else {
        console.error('Failed to send message:', error);
        setMessages(prev => {
          const newMessages = [...prev];
          newMessages[newMessages.length - 1] = {
            role: 'assistant',
            content: buildErrorMessage(error),
          };
          return newMessages;
        });
      }
    } finally {
      reset();
      abortControllerRef.current = null;
      userScrolledAwayRef.current = false;
      window.dispatchEvent(new CustomEvent('inference-complete'));
    }
  };

  const submitEdit = async () => {
    if ((!editingValue.trim() && editingImages.length === 0) || editingIndex === null || isBusy) return;

    const ready = await runPreFlight('llm', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    if (abortControllerRef.current) abortControllerRef.current.abort();
    abortControllerRef.current = new AbortController();
    setIsUserAtBottom(true);
    userScrolledAwayRef.current = false;

    const truncatedMessages = messages.slice(0, editingIndex);

    let messageContent: MessageContent;
    if (editingImages.length > 0) {
      const contentArray: Array<TextContent | ImageContent> = [];
      if (editingValue.trim()) contentArray.push({ type: 'text', text: editingValue });
      editingImages.forEach(imageUrl => {
        contentArray.push({ type: 'image_url', image_url: { url: imageUrl } });
      });
      messageContent = contentArray;
    } else {
      messageContent = editingValue;
    }

    const editedMessage: Message = { role: 'user', content: messageContent };
    const messageHistory = [...truncatedMessages, editedMessage];

    setMessages(messageHistory);
    setEditingIndex(null);
    setEditingValue('');
    setEditingImages([]);
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    try {
      await handleStreamingResponse(messageHistory);
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        setMessages(prev => {
          const lastMessage = prev[prev.length - 1];
          if (!lastMessage || (!lastMessage.content && !lastMessage.thinking)) return prev.slice(0, -1);
          return prev;
        });
      } else {
        console.error('Failed to send message:', error);
        setMessages(prev => {
          const newMessages = [...prev];
          newMessages[newMessages.length - 1] = {
            role: 'assistant',
            content: buildErrorMessage(error),
          };
          return newMessages;
        });
      }
    } finally {
      reset();
      abortControllerRef.current = null;
      userScrolledAwayRef.current = false;
      window.dispatchEvent(new CustomEvent('inference-complete'));
    }
  };

  const handleKeyPress = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  };

  const handleInputChange = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    setInputValue(e.target.value);
    adjustTextareaHeight(e.target);
  };

  const handleStopGeneration = () => {
    if (abortControllerRef.current) abortControllerRef.current.abort();
  };

  const toggleThinking = (index: number) => {
    setExpandedThinking(prev => {
      const next = new Set(prev);
      if (next.has(index)) next.delete(index);
      else next.add(index);
      return next;
    });
  };

  const renderMessageContent = (content: MessageContent, thinking?: string, messageIndex?: number, isComplete?: boolean) => (
    <>
      {thinking && (
        <div className="thinking-section">
          <button
            className="thinking-toggle"
            onClick={() => messageIndex !== undefined && toggleThinking(messageIndex)}
          >
            <svg
              width="12" height="12" viewBox="0 0 24 24" fill="none"
              style={{
                transform: expandedThinking.has(messageIndex!) ? 'rotate(180deg)' : 'rotate(0deg)',
                transition: 'transform 0.2s',
              }}
            >
              <path d="M6 9L12 15L18 9" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
            </svg>
            <span>Thinking</span>
          </button>
          {expandedThinking.has(messageIndex!) && (
            <div className="thinking-content">
              <MarkdownMessage content={thinking} isComplete={isComplete} />
            </div>
          )}
        </div>
      )}
      {typeof content === 'string' ? (
        <MarkdownMessage content={content} isComplete={isComplete} />
      ) : (
        <div className="message-content-array">
          {content.map((item, index) => {
            if (item.type === 'text') return <MarkdownMessage key={index} content={item.text} isComplete={isComplete} />;
            if (item.type === 'image_url') return <img key={index} src={item.image_url.url} alt="Uploaded" className="message-image" />;
            return null;
          })}
        </div>
      )}
    </>
  );

  const handleEditMessage = (index: number, e: React.MouseEvent) => {
    if (isBusy) return;
    e.stopPropagation();
    const message = messages[index];
    if (message.role === 'user') {
      setEditingIndex(index);
      if (typeof message.content === 'string') {
        setEditingValue(message.content);
        setEditingImages([]);
      } else {
        const textContent = message.content.find(item => item.type === 'text');
        setEditingValue(textContent ? textContent.text : '');
        const imageContents = message.content.filter(item => item.type === 'image_url');
        setEditingImages(imageContents.map(img => img.image_url.url));
      }
    }
  };

  const handleEditInputChange = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    setEditingValue(e.target.value);
    e.target.style.height = 'auto';
    e.target.style.height = e.target.scrollHeight + 'px';
  };

  const cancelEdit = () => {
    setEditingIndex(null);
    setEditingValue('');
    setEditingImages([]);
  };

  const handleEditContainerClick = (e: React.MouseEvent) => {
    e.stopPropagation();
  };

  const handleEditKeyPress = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      submitEdit();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      cancelEdit();
    }
  };

  const renderAudioButton = (role: string, message: MessageContent, btnIndex: number) => {
    let isTextContent = (typeof message === 'object')
      ? (message.filter((chunk) => chunk.type === 'text').length != 0)
      : true;

    return (appSettings?.tts.enableTTS.value) &&
      isTextContent &&
      ((role == 'assistant') ||
      (role == 'user' && appSettings?.tts.enableUserTTS.value)) ?
      <AudioButton
        role={role}
        textMessage={message}
        buttonIndex={btnIndex}
        onClickFunction={tts.handleAudioButtonClick}
        buttonContext={{ buttonId: tts.pressedAudioButton, audioState: tts.audioState }}
      /> :
      '';
  };

  return (
    <>
      <div
        className="chat-messages"
        ref={messagesContainerRef}
        onScroll={handleScroll}
        onClick={editingIndex !== null ? cancelEdit : undefined}
      >
        {messages.length === 0 && <EmptyState title="Lemonade Chat" />}
        {messages.map((message, index) => {
          const isGrayedOut = editingIndex !== null && index > editingIndex;
          return (
            <div
              key={index}
              className={`chat-message ${message.role === 'user' ? 'user-message' : 'assistant-message'} ${
                message.role === 'user' && !isBusy ? 'editable' : ''
              } ${isGrayedOut ? 'grayed-out' : ''} ${editingIndex === index ? 'editing' : ''}`}
            >
              {renderAudioButton(message.role, message.content, index)}
              {editingIndex === index ? (
                <div className="edit-message-wrapper" onClick={handleEditContainerClick}>
                  <ImagePreviewList
                    images={editingImages}
                    onRemove={editingImageHandlers.remove}
                    altPrefix="Edit"
                    className="edit-image-preview-container"
                  />
                  <div className="edit-message-content">
                    <textarea
                      ref={editTextareaRef}
                      className="edit-message-input"
                      value={editingValue}
                      onChange={handleEditInputChange}
                      onKeyDown={handleEditKeyPress}
                      onPaste={editingImageHandlers.paste}
                      autoFocus
                      rows={1}
                    />
                    <div className="edit-message-controls">
                      {isVision && (
                        <>
                          <input
                            ref={editFileInputRef}
                            type="file"
                            accept="image/*"
                            onChange={editingImageHandlers.upload}
                            style={{ display: 'none' }}
                          />
                          <button
                            className="image-upload-button"
                            onClick={() => editFileInputRef.current?.click()}
                            title="Upload image"
                          >
                            <ImageUploadIcon />
                          </button>
                        </>
                      )}
                      <button
                        className="edit-send-button"
                        onClick={submitEdit}
                        disabled={!editingValue.trim() && editingImages.length === 0}
                        title="Send edited message"
                      >
                        <SendIcon />
                      </button>
                    </div>
                  </div>
                </div>
              ) : (
                <div
                  onClick={(e) => message.role === 'user' && !isBusy && handleEditMessage(index, e)}
                  style={{ cursor: message.role === 'user' && !isBusy ? 'pointer' : 'default' }}
                >
                  {renderMessageContent(message.content, message.thinking, index, message.role === 'assistant')}
                </div>
              )}
            </div>
          );
        })}
        {isPreFlight && activeModality === 'llm' && (
          <div className="model-loading-indicator">
            <span className="model-loading-text">Loading model</span>
          </div>
        )}
        {isInferring && activeModality === 'llm' && (
          <div className="chat-message assistant-message">
            <TypingIndicator />
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          <ImagePreviewList
            images={uploadedImages}
            onRemove={uploadedImageHandlers.remove}
          />
          <textarea
            ref={inputTextareaRef}
            className="chat-input"
            value={inputValue}
            onChange={handleInputChange}
            onKeyPress={handleKeyPress}
            onPaste={uploadedImageHandlers.paste}
            placeholder="Type your message..."
            rows={1}
          />
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={true}
            onSend={sendMessage}
            onStop={handleStopGeneration}
            sendDisabled={!inputValue.trim() && uploadedImages.length === 0}
            modelSelector={<ModelSelector disabled={isBusy} />}
            leftControls={
              isVision ? (
                <>
                  <input
                    ref={fileInputRef}
                    type="file"
                    accept="image/*"
                    onChange={uploadedImageHandlers.upload}
                    style={{ display: 'none' }}
                  />
                  <button
                    className="image-upload-button"
                    onClick={() => fileInputRef.current?.click()}
                    disabled={isBusy}
                    title="Upload image"
                  >
                    <ImageUploadIcon />
                  </button>
                </>
              ) : undefined
            }
          />
        </div>
      </div>
    </>
  );
};

export default LLMChatPanel;
