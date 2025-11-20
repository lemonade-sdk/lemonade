import React, { useState, useEffect, useRef } from 'react';
import MarkdownMessage from './MarkdownMessage';
import serverModelsData from '../../../lemonade_server/server_models.json';
import logoSvg from '../../assets/logo.svg';

const CHAT_API_BASE = 'http://localhost:8000/api/v1';

interface ImageContent {
  type: 'image_url';
  image_url: {
    url: string;
  };
}

interface TextContent {
  type: 'text';
  text: string;
}

type MessageContent = string | Array<TextContent | ImageContent>;

interface Message {
  role: 'user' | 'assistant';
  content: MessageContent;
  thinking?: string;
}

interface Model {
  id: string;
  object: string;
  created?: number;
  owned_by?: string;
}

interface ModelInfo {
  checkpoint: string;
  recipe: string;
  suggested: boolean;
  size: number;
  labels?: string[];
  max_prompt_length?: number;
  mmproj?: string;
}

interface ModelsData {
  [key: string]: ModelInfo;
}

interface ChatWindowProps {
  isVisible: boolean;
  width?: number;
}

const ChatWindow: React.FC<ChatWindowProps> = ({ isVisible, width }) => {
  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [models, setModels] = useState<Model[]>([]);
  const [selectedModel, setSelectedModel] = useState('');
  const [isLoading, setIsLoading] = useState(false);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [editingImages, setEditingImages] = useState<string[]>([]);
  const [uploadedImages, setUploadedImages] = useState<string[]>([]);
  const [expandedThinking, setExpandedThinking] = useState<Set<number>>(new Set());
  const [isUserAtBottom, setIsUserAtBottom] = useState(true);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const editFileInputRef = useRef<HTMLInputElement>(null);
  const abortControllerRef = useRef<AbortController | null>(null);
  const scrollTimeoutRef = useRef<NodeJS.Timeout | null>(null);

useEffect(() => {
  fetchModels();
  fetchLoadedModel();

  const handleModelLoadEnd = (event: Event) => {
    const customEvent = event as CustomEvent<{ modelId?: string }>;
    const loadedModelId = customEvent.detail?.modelId;

    if (loadedModelId) {
      setSelectedModel(loadedModelId);
    } else {
      fetchLoadedModel();
    }

    // Refresh the models list so newly loaded models appear in the dropdown
    fetchModels();
  };

  window.addEventListener('modelLoadEnd' as any, handleModelLoadEnd);

  return () => {
    window.removeEventListener('modelLoadEnd' as any, handleModelLoadEnd);
  };
}, []);

  useEffect(() => {
    // Only auto-scroll if user is at the bottom
    if (isUserAtBottom) {
      if (scrollTimeoutRef.current) {
        clearTimeout(scrollTimeoutRef.current);
      }
      
      // Use requestAnimationFrame to scroll after render completes
      requestAnimationFrame(() => {
        scrollToBottom();
      });
    }
    
    return () => {
      if (scrollTimeoutRef.current) {
        clearTimeout(scrollTimeoutRef.current);
      }
    };
  }, [messages, isLoading, isUserAtBottom]);

  useEffect(() => {
    if (editTextareaRef.current) {
      editTextareaRef.current.style.height = 'auto';
      editTextareaRef.current.style.height = editTextareaRef.current.scrollHeight + 'px';
    }
  }, [editingIndex, editingValue]);

  const checkIfAtBottom = () => {
    const container = messagesContainerRef.current;
    if (!container) return true;
    
    const threshold = 20; // pixels from bottom to consider "at bottom"
    const isAtBottom = container.scrollHeight - container.scrollTop - container.clientHeight < threshold;
    return isAtBottom;
  };

  const handleScroll = () => {
    const atBottom = checkIfAtBottom();
    setIsUserAtBottom(atBottom);
  };

  const scrollToBottom = () => {
    messagesEndRef.current?.scrollIntoView({ behavior: isLoading ? 'auto' : 'smooth' });
    setIsUserAtBottom(true);
  };

const fetchModels = async () => {
  try {
    const response = await fetch(`${CHAT_API_BASE}/models`);
    const data = await response.json();
    
    // Handle both array format and object with data array
    const modelList = Array.isArray(data) ? data : data.data || [];
    setModels(modelList);
    
    if (modelList.length > 0) {
      setSelectedModel(prev => prev || modelList[0].id);
    }
  } catch (error) {
    console.error('Failed to fetch models:', error);
  }
};

const fetchLoadedModel = async () => {
  try {
    const response = await fetch(`${CHAT_API_BASE}/health`);
    const data = await response.json();

    if (data?.model_loaded) {
      setSelectedModel(data.model_loaded);
    }
  } catch (error) {
    console.error('Failed to fetch loaded model:', error);
  }
};

  const isVisionModel = (): boolean => {
    if (!selectedModel) return false;
    
    const modelsData: ModelsData = serverModelsData as ModelsData;
    const modelInfo = modelsData[selectedModel];
    
    return modelInfo?.labels?.includes('vision') || false;
  };

  const handleImageUpload = (event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;
    if (!files || files.length === 0) return;

    const file = files[0];
    const reader = new FileReader();
    
    reader.onload = (e) => {
      const result = e.target?.result;
      if (typeof result === 'string') {
        setUploadedImages(prev => [...prev, result]);
      }
    };
    
    reader.readAsDataURL(file);
  };

  const handleImagePaste = (event: React.ClipboardEvent) => {
    const items = event.clipboardData.items;
    
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      
      if (item.type.indexOf('image') !== -1) {
        event.preventDefault();
        const file = item.getAsFile();
        if (!file) continue;
        
        const reader = new FileReader();
        reader.onload = (e) => {
          const result = e.target?.result;
          if (typeof result === 'string') {
            setUploadedImages(prev => [...prev, result]);
          }
        };
        reader.readAsDataURL(file);
        break;
      }
    }
  };

  const removeImage = (index: number) => {
    setUploadedImages(prev => prev.filter((_, i) => i !== index));
  };

  const sendMessage = async () => {
    if ((!inputValue.trim() && uploadedImages.length === 0) || isLoading) return;

    // Cancel any existing request
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }

    // Create new abort controller
    abortControllerRef.current = new AbortController();
    
    // When sending a new message, ensure we're at the bottom
    setIsUserAtBottom(true);

    // Build message content with images if present
    let messageContent: MessageContent;
    if (uploadedImages.length > 0) {
      const contentArray: Array<TextContent | ImageContent> = [];
      
      if (inputValue.trim()) {
        contentArray.push({
          type: 'text',
          text: inputValue
        });
      }
      
      uploadedImages.forEach(imageUrl => {
        contentArray.push({
          type: 'image_url',
          image_url: {
            url: imageUrl
          }
        });
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
    setIsLoading(true);

    // Add placeholder for assistant message
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    let accumulatedContent = '';
    let accumulatedThinking = '';

    try {
      const response = await fetch(`${CHAT_API_BASE}/chat/completions`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          model: selectedModel,
          messages: messageHistory,
          stream: true,
        }),
        signal: abortControllerRef.current.signal,
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      if (!response.body) {
        throw new Error('Response body is null');
      }

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
              
              if (data === '[DONE]') {
                continue;
              }

              if (!data) {
                continue;
              }

              try {
                const parsed = JSON.parse(data);
                const delta = parsed.choices?.[0]?.delta;
                const content = delta?.content;
                const thinkingContent = delta?.reasoning_content || delta?.thinking;
                
                if (content) {
                  accumulatedContent += content;
                }
                
                if (thinkingContent) {
                  accumulatedThinking += thinkingContent;
                }
                
                if (content || thinkingContent) {
                  setMessages(prev => {
                    const newMessages = [...prev];
                    const messageIndex = newMessages.length - 1;
                    newMessages[messageIndex] = {
                      role: 'assistant',
                      content: accumulatedContent,
                      thinking: accumulatedThinking || undefined,
                    };
                    
                    // Auto-expand thinking section if thinking content is present
                    if (accumulatedThinking) {
                      setExpandedThinking(prevExpanded => {
                        const next = new Set(prevExpanded);
                        next.add(messageIndex);
                        return next;
                      });
                    }
                    
                    return newMessages;
                  });
                }
              } catch (e) {
                console.warn('Failed to parse SSE data:', data, e);
              }
            }
          }
        }
      } finally {
        reader.releaseLock();
      }

      if (!accumulatedContent) {
        throw new Error('No content received from stream');
      }
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        // Keep the partial message that was received
        // If no content was received, remove the empty message
        if (!accumulatedContent && !accumulatedThinking) {
          setMessages(prev => prev.slice(0, -1));
        }
      } else {
        console.error('Failed to send message:', error);
        setMessages(prev => {
          const newMessages = [...prev];
          newMessages[newMessages.length - 1] = {
            role: 'assistant',
            content: `Error: ${error.message || 'Failed to get response from the model.'}`,
          };
          return newMessages;
        });
      }
    } finally {
      setIsLoading(false);
      abortControllerRef.current = null;
    }
  };

  const handleKeyPress = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  };

  const toggleThinking = (index: number) => {
    setExpandedThinking(prev => {
      const next = new Set(prev);
      if (next.has(index)) {
        next.delete(index);
      } else {
        next.add(index);
      }
      return next;
    });
  };

  const renderMessageContent = (content: MessageContent, thinking?: string, messageIndex?: number) => {
    return (
      <>
        {thinking && (
          <div className="thinking-section">
            <button 
              className="thinking-toggle"
              onClick={() => messageIndex !== undefined && toggleThinking(messageIndex)}
            >
              <svg 
                width="12" 
                height="12" 
                viewBox="0 0 24 24" 
                fill="none"
                style={{ 
                  transform: expandedThinking.has(messageIndex!) ? 'rotate(180deg)' : 'rotate(0deg)',
                  transition: 'transform 0.2s'
                }}
              >
                <path 
                  d="M6 9L12 15L18 9" 
                  stroke="currentColor" 
                  strokeWidth="2" 
                  strokeLinecap="round" 
                  strokeLinejoin="round"
                />
              </svg>
              <span>Thinking</span>
            </button>
            {expandedThinking.has(messageIndex!) && (
              <div className="thinking-content">
                <MarkdownMessage content={thinking} />
              </div>
            )}
          </div>
        )}
        {typeof content === 'string' ? (
          <MarkdownMessage content={content} />
        ) : (
          <div className="message-content-array">
            {content.map((item, index) => {
              if (item.type === 'text') {
                return <MarkdownMessage key={index} content={item.text} />;
              } else if (item.type === 'image_url') {
                return (
                  <img 
                    key={index} 
                    src={item.image_url.url} 
                    alt="Uploaded" 
                    className="message-image"
                  />
                );
              }
              return null;
            })}
          </div>
        )}
      </>
    );
  };

  const handleEditMessage = (index: number, e: React.MouseEvent) => {
    if (isLoading) return; // Don't allow editing while loading
    
    e.stopPropagation(); // Prevent triggering the outside click
    const message = messages[index];
    if (message.role === 'user') {
      setEditingIndex(index);
      // Extract text and image content from message
      if (typeof message.content === 'string') {
        setEditingValue(message.content);
        setEditingImages([]);
      } else {
        // If content is an array, extract the text and image parts
        const textContent = message.content.find(item => item.type === 'text');
        setEditingValue(textContent ? textContent.text : '');
        
        const imageContents = message.content.filter(item => item.type === 'image_url');
        setEditingImages(imageContents.map(img => img.image_url.url));
      }
    }
  };

  const handleEditInputChange = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    setEditingValue(e.target.value);
    // Auto-grow the textarea
    e.target.style.height = 'auto';
    e.target.style.height = e.target.scrollHeight + 'px';
  };

  const cancelEdit = () => {
    setEditingIndex(null);
    setEditingValue('');
    setEditingImages([]);
  };

  const handleEditImageUpload = (event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;
    if (!files || files.length === 0) return;

    const file = files[0];
    const reader = new FileReader();
    
    reader.onload = (e) => {
      const result = e.target?.result;
      if (typeof result === 'string') {
        setEditingImages(prev => [...prev, result]);
      }
    };
    
    reader.readAsDataURL(file);
  };

  const handleEditImagePaste = (event: React.ClipboardEvent) => {
    const items = event.clipboardData.items;
    
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      
      if (item.type.indexOf('image') !== -1) {
        event.preventDefault();
        const file = item.getAsFile();
        if (!file) continue;
        
        const reader = new FileReader();
        reader.onload = (e) => {
          const result = e.target?.result;
          if (typeof result === 'string') {
            setEditingImages(prev => [...prev, result]);
          }
        };
        reader.readAsDataURL(file);
        break;
      }
    }
  };

  const removeEditImage = (index: number) => {
    setEditingImages(prev => prev.filter((_, i) => i !== index));
  };

  // Handle click outside to cancel edit
  const handleEditContainerClick = (e: React.MouseEvent) => {
    e.stopPropagation(); // Prevent closing when clicking inside the edit area
  };

  const submitEdit = async () => {
    if ((!editingValue.trim() && editingImages.length === 0) || editingIndex === null || isLoading) return;

    // Cancel any existing request
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }

    // Create new abort controller
    abortControllerRef.current = new AbortController();
    
    // When submitting an edit, ensure we're at the bottom
    setIsUserAtBottom(true);

    // Truncate messages up to the edited message
    const truncatedMessages = messages.slice(0, editingIndex);
    
    // Build edited message content with images if present
    let messageContent: MessageContent;
    if (editingImages.length > 0) {
      const contentArray: Array<TextContent | ImageContent> = [];
      
      if (editingValue.trim()) {
        contentArray.push({
          type: 'text',
          text: editingValue
        });
      }
      
      editingImages.forEach(imageUrl => {
        contentArray.push({
          type: 'image_url',
          image_url: {
            url: imageUrl
          }
        });
      });
      
      messageContent = contentArray;
    } else {
      messageContent = editingValue;
    }
    
    // Add the edited message
    const editedMessage: Message = { role: 'user', content: messageContent };
    const messageHistory = [...truncatedMessages, editedMessage];
    
    setMessages(messageHistory);
    setEditingIndex(null);
    setEditingValue('');
    setEditingImages([]);
    setIsLoading(true);

    // Add placeholder for assistant message
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    let accumulatedContent = '';
    let accumulatedThinking = '';

    try {
      const response = await fetch(`${CHAT_API_BASE}/chat/completions`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          model: selectedModel,
          messages: messageHistory,
          stream: true,
        }),
        signal: abortControllerRef.current.signal,
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      if (!response.body) {
        throw new Error('Response body is null');
      }

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
              
              if (data === '[DONE]') {
                continue;
              }

              if (!data) {
                continue;
              }

              try {
                const parsed = JSON.parse(data);
                const delta = parsed.choices?.[0]?.delta;
                const content = delta?.content;
                const thinkingContent = delta?.reasoning_content || delta?.thinking;
                
                if (content) {
                  accumulatedContent += content;
                }
                
                if (thinkingContent) {
                  accumulatedThinking += thinkingContent;
                }
                
                if (content || thinkingContent) {
                  setMessages(prev => {
                    const newMessages = [...prev];
                    const messageIndex = newMessages.length - 1;
                    newMessages[messageIndex] = {
                      role: 'assistant',
                      content: accumulatedContent,
                      thinking: accumulatedThinking || undefined,
                    };
                    
                    // Auto-expand thinking section if thinking content is present
                    if (accumulatedThinking) {
                      setExpandedThinking(prevExpanded => {
                        const next = new Set(prevExpanded);
                        next.add(messageIndex);
                        return next;
                      });
                    }
                    
                    return newMessages;
                  });
                }
              } catch (e) {
                console.warn('Failed to parse SSE data:', data, e);
              }
            }
          }
        }
      } finally {
        reader.releaseLock();
      }

      if (!accumulatedContent) {
        throw new Error('No content received from stream');
      }
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        // Keep the partial message that was received
        // If no content was received, remove the empty message
        if (!accumulatedContent && !accumulatedThinking) {
          setMessages(prev => prev.slice(0, -1));
        }
      } else {
        console.error('Failed to send message:', error);
        setMessages(prev => {
          const newMessages = [...prev];
          newMessages[newMessages.length - 1] = {
            role: 'assistant',
            content: `Error: ${error.message || 'Failed to get response from the model.'}`,
          };
          return newMessages;
        });
      }
    } finally {
      setIsLoading(false);
      abortControllerRef.current = null;
    }
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

  const handleStopGeneration = () => {
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }
  };

  const handleNewChat = () => {
    // Cancel any ongoing request
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }
    
    // Clear all messages and reset state
    setMessages([]);
    setInputValue('');
    setUploadedImages([]);
    setEditingIndex(null);
    setEditingValue('');
    setEditingImages([]);
    setIsLoading(false);
    setExpandedThinking(new Set());
    setIsUserAtBottom(true);
  };

  if (!isVisible) return null;

  return (
    <div className="chat-window" style={width ? { width: `${width}px` } : undefined}>
      <div className="chat-header">
        <h3>LLM Chat</h3>
        <button 
          className="new-chat-button"
          onClick={handleNewChat}
          disabled={isLoading}
          title="Start a new chat"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
            <path
              d="M21 12C21 16.9706 16.9706 21 12 21C9.69494 21 7.59227 20.1334 6 18.7083L3 21V14H10L7.45508 16.5449C8.63695 17.4677 10.1453 18 11.7614 18C15.8777 18 19.2614 14.6163 19.2614 10.5C19.2614 6.38367 15.8777 3 11.7614 3C9.15539 3 6.87646 4.47203 5.70638 6.60081L3.45789 5.45704C5.08003 2.56887 8.15536 0.5 11.7614 0.5C17.2842 0.5 21.7614 4.97716 21.7614 10.5C21.7614 10.8343 21.7457 11.1651 21.7152 11.4914"
              stroke="currentColor"
              strokeWidth="2"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
          </svg>
        </button>
      </div>

      <div 
        className="chat-messages" 
        ref={messagesContainerRef}
        onScroll={handleScroll}
        onClick={editingIndex !== null ? cancelEdit : undefined}
      >
        {messages.length === 0 && (
          <div className="chat-empty-state">
            <img 
              src={logoSvg} 
              alt="Lemonade Logo" 
              className="chat-empty-logo"
            />
            <h2 className="chat-empty-title">Lemonade Chat</h2>
          </div>
        )}
        {messages.map((message, index) => {
          const isGrayedOut = editingIndex !== null && index > editingIndex;
          return (
            <div
              key={index}
              className={`chat-message ${message.role === 'user' ? 'user-message' : 'assistant-message'} ${
                message.role === 'user' && !isLoading ? 'editable' : ''
              } ${isGrayedOut ? 'grayed-out' : ''} ${editingIndex === index ? 'editing' : ''}`}
            >
              {editingIndex === index ? (
                <div className="edit-message-wrapper" onClick={handleEditContainerClick}>
                  {editingImages.length > 0 && (
                    <div className="edit-image-preview-container">
                      {editingImages.map((imageUrl, imgIndex) => (
                        <div key={imgIndex} className="image-preview-item">
                          <img src={imageUrl} alt={`Edit ${imgIndex + 1}`} className="image-preview" />
                          <button
                            className="image-remove-button"
                            onClick={() => removeEditImage(imgIndex)}
                            title="Remove image"
                          >
                            ×
                          </button>
                        </div>
                      ))}
                    </div>
                  )}
                  <div className="edit-message-content">
                    <textarea
                      ref={editTextareaRef}
                      className="edit-message-input"
                      value={editingValue}
                      onChange={handleEditInputChange}
                      onKeyDown={handleEditKeyPress}
                      onPaste={handleEditImagePaste}
                      autoFocus
                      rows={1}
                    />
                    <div className="edit-message-controls">
                      {isVisionModel() && (
                        <>
                          <input
                            ref={editFileInputRef}
                            type="file"
                            accept="image/*"
                            onChange={handleEditImageUpload}
                            style={{ display: 'none' }}
                          />
                          <button
                            className="image-upload-button"
                            onClick={() => editFileInputRef.current?.click()}
                            title="Upload image"
                          >
                            <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                              <path
                                d="M21 19V5C21 3.9 20.1 3 19 3H5C3.9 3 3 3.9 3 5V19C3 20.1 3.9 21 5 21H19C20.1 21 21 20.1 21 19ZM8.5 13.5L11 16.51L14.5 12L19 18H5L8.5 13.5Z"
                                fill="currentColor"
                              />
                            </svg>
                          </button>
                        </>
                      )}
                      <button
                        className="edit-send-button"
                        onClick={submitEdit}
                        disabled={!editingValue.trim() && editingImages.length === 0}
                        title="Send edited message"
                      >
                        <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                          <path
                            d="M22 2L11 13M22 2L15 22L11 13M22 2L2 9L11 13"
                            stroke="currentColor"
                            strokeWidth="2"
                            strokeLinecap="round"
                            strokeLinejoin="round"
                          />
                        </svg>
                      </button>
                    </div>
                  </div>
                </div>
              ) : (
                <div
                  onClick={(e) => message.role === 'user' && !isLoading && handleEditMessage(index, e)}
                  style={{ cursor: message.role === 'user' && !isLoading ? 'pointer' : 'default' }}
                >
                  {renderMessageContent(message.content, message.thinking, index)}
                </div>
              )}
            </div>
          );
        })}
        {isLoading && (
          <div className="chat-message assistant-message">
            <div className="typing-indicator">
              <span></span>
              <span></span>
              <span></span>
            </div>
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          {uploadedImages.length > 0 && (
            <div className="image-preview-container">
              {uploadedImages.map((imageUrl, index) => (
                <div key={index} className="image-preview-item">
                  <img src={imageUrl} alt={`Upload ${index + 1}`} className="image-preview" />
                  <button
                    className="image-remove-button"
                    onClick={() => removeImage(index)}
                    title="Remove image"
                  >
                    ×
                  </button>
                </div>
              ))}
            </div>
          )}
          <textarea
            ref={inputTextareaRef}
            className="chat-input"
            value={inputValue}
            onChange={(e) => setInputValue(e.target.value)}
            onKeyPress={handleKeyPress}
            onPaste={handleImagePaste}
            placeholder="Type your message..."
            rows={1}
            disabled={isLoading}
          />
          <div className="chat-controls">
            <div className="chat-controls-left">
              {isVisionModel() && (
                <>
                  <input
                    ref={fileInputRef}
                    type="file"
                    accept="image/*"
                    onChange={handleImageUpload}
                    style={{ display: 'none' }}
                  />
                  <button
                    className="image-upload-button"
                    onClick={() => fileInputRef.current?.click()}
                    disabled={isLoading}
                    title="Upload image"
                  >
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                      <path
                        d="M21 19V5C21 3.9 20.1 3 19 3H5C3.9 3 3 3.9 3 5V19C3 20.1 3.9 21 5 21H19C20.1 21 21 20.1 21 19ZM8.5 13.5L11 16.51L14.5 12L19 18H5L8.5 13.5Z"
                        fill="currentColor"
                      />
                    </svg>
                  </button>
                </>
              )}
              <select
                className="model-selector"
                value={selectedModel}
                onChange={(e) => setSelectedModel(e.target.value)}
                disabled={isLoading}
              >
                {models.map((model) => (
                  <option key={model.id} value={model.id}>
                    {model.id}
                  </option>
                ))}
              </select>
            </div>
            {isLoading ? (
              <button
                className="chat-stop-button"
                onClick={handleStopGeneration}
                title="Stop generation"
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                  <rect
                    x="6"
                    y="6"
                    width="12"
                    height="12"
                    fill="currentColor"
                    rx="2"
                  />
                </svg>
              </button>
            ) : (
              <button
                className="chat-send-button"
                onClick={sendMessage}
                disabled={!inputValue.trim() && uploadedImages.length === 0}
                title="Send message"
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                  <path
                    d="M22 2L11 13M22 2L15 22L11 13M22 2L2 9L11 13"
                    stroke="currentColor"
                    strokeWidth="2"
                    strokeLinecap="round"
                    strokeLinejoin="round"
                  />
                </svg>
              </button>
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

export default ChatWindow;


