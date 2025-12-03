import React, { useState, useEffect, useRef, useCallback } from 'react';
import MarkdownMessage from './MarkdownMessage';
import { fetchSupportedModelsData, ModelsData } from './utils/modelData';
// @ts-ignore - SVG assets live outside of the TypeScript rootDir for Electron packaging
import logoSvg from '../../assets/logo.svg';
import {
  AppSettings,
  buildChatRequestOverrides,
  mergeWithDefaultSettings,
} from './utils/appSettings';
import { serverFetch, onServerPortChange } from './utils/serverConfig';

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

interface ChatWindowProps {
  isVisible: boolean;
  width?: number;
}

const ChatWindow: React.FC<ChatWindowProps> = ({ isVisible, width }) => {
  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [models, setModels] = useState<Model[]>([]);
  const [supportedModelsData, setSupportedModelsData] = useState<ModelsData>({});
  const [selectedModel, setSelectedModel] = useState('');
  const [isLoading, setIsLoading] = useState(false);
  const [currentLoadedModel, setCurrentLoadedModel] = useState<string | null>(null);
  const [isModelLoading, setIsModelLoading] = useState(false);
  // Track if user has manually selected a model (to avoid overriding their choice)
  const userHasSelectedModelRef = useRef(false);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [editingImages, setEditingImages] = useState<string[]>([]);
  const [uploadedImages, setUploadedImages] = useState<string[]>([]);
  const [expandedThinking, setExpandedThinking] = useState<Set<number>>(new Set());
  const [isUserAtBottom, setIsUserAtBottom] = useState(true);
  const userScrolledAwayRef = useRef(false); // Immediate tracking for scroll during streaming
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const editFileInputRef = useRef<HTMLInputElement>(null);
  const abortControllerRef = useRef<AbortController | null>(null);
  const scrollTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  const [appSettings, setAppSettings] = useState<AppSettings | null>(null);

useEffect(() => {
  fetchModels();
  fetchLoadedModel();
  fetchSupportedModels();
  const loadSettings = async () => {
    if (!window.api?.getSettings) {
      return;
    }

    try {
      const stored = await window.api.getSettings();
      setAppSettings(mergeWithDefaultSettings(stored));
    } catch (error) {
      console.error('Failed to load app settings:', error);
    }
  };

  loadSettings();

  const unsubscribeSettings = window.api?.onSettingsUpdated?.((updated) => {
    setAppSettings(mergeWithDefaultSettings(updated));
  });

  const handleModelLoadEnd = (event: Event) => {
    const customEvent = event as CustomEvent<{ modelId?: string }>;
    const loadedModelId = customEvent.detail?.modelId;

    // Update the current loaded model state
    if (loadedModelId) {
      setCurrentLoadedModel(loadedModelId);
      setIsModelLoading(false);
    }

    // When a model is explicitly loaded (via Model Manager or other explicit action),
    // always select it in the chat - this is an intentional user action
    if (loadedModelId) {
      setSelectedModel(loadedModelId);
      // Reset the manual selection flag since the user loaded a new model
      userHasSelectedModelRef.current = false;
    } else {
      // Fallback: fetch the loaded model from the health endpoint
      fetchLoadedModel();
    }

    // Refresh the models list so newly loaded models appear in the dropdown
    fetchModels();
  };

  const handleModelUnload = () => {
    // Model was unloaded/ejected - reset the current loaded model state
    setCurrentLoadedModel(null);
  };

  window.addEventListener('modelLoadEnd' as any, handleModelLoadEnd);
  window.addEventListener('modelUnload' as any, handleModelUnload);

  // Periodically check health status to detect when another app unloads the model
  const healthCheckInterval = setInterval(() => {
    fetchLoadedModel();
  }, 5000);

  // Listen for port changes and refetch data
  const unsubscribePortChange = onServerPortChange(() => {
    console.log('Server port changed, refetching chat data...');
    fetchModels();
    fetchLoadedModel();
  });

  return () => {
    window.removeEventListener('modelLoadEnd' as any, handleModelLoadEnd);
    window.removeEventListener('modelUnload' as any, handleModelUnload);
    clearInterval(healthCheckInterval);
    unsubscribePortChange();
    if (typeof unsubscribeSettings === 'function') {
      unsubscribeSettings();
    }
  };
}, []);

  useEffect(() => {
    // Only auto-scroll if user hasn't scrolled away during streaming
    // Use the ref for immediate check to prevent overriding user scroll
    if (!userScrolledAwayRef.current && isUserAtBottom) {
      if (scrollTimeoutRef.current) {
        clearTimeout(scrollTimeoutRef.current);
      }
      
      // Use requestAnimationFrame to scroll after render completes
      requestAnimationFrame(() => {
        if (!userScrolledAwayRef.current) {
          scrollToBottom();
        }
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
    
    // Track immediately via ref - if user scrolls away during streaming, respect it
    if (!atBottom && isLoading) {
      userScrolledAwayRef.current = true;
    } else if (atBottom) {
      // User scrolled back to bottom, reset the flag
      userScrolledAwayRef.current = false;
    }
  };

  const scrollToBottom = () => {
    messagesEndRef.current?.scrollIntoView({ behavior: isLoading ? 'auto' : 'smooth' });
    setIsUserAtBottom(true);
  };

const fetchModels = async () => {
  try {
    const response = await serverFetch('/models');
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
    const response = await serverFetch('/health');
    const data = await response.json();

    if (data?.model_loaded) {
      setCurrentLoadedModel(data.model_loaded);
      // Only auto-select if user hasn't manually chosen a model
      if (!userHasSelectedModelRef.current) {
        setSelectedModel(data.model_loaded);
      }
      // If the model we were waiting for is now loaded, clear the loading state
      setIsModelLoading(false);
    } else {
      setCurrentLoadedModel(null);
    }
  } catch (error) {
    console.error('Failed to fetch loaded model:', error);
  }
};

const fetchSupportedModels = useCallback(async () => {
  try {
    const data = await fetchSupportedModelsData();
    setSupportedModelsData(data);
  } catch (error) {
    console.error('Failed to load supported models:', error);
  }
}, []);

useEffect(() => {
  if (!window.api?.watchUserModels) {
    return;
  }

  const stopWatching = window.api.watchUserModels(() => {
    fetchSupportedModels();
  });

  return () => {
    if (typeof stopWatching === 'function') {
      stopWatching();
    }
  };
}, [fetchSupportedModels]);

  const isVisionModel = (): boolean => {
    if (!selectedModel) return false;
    
    const modelInfo = supportedModelsData[selectedModel];
    
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

const buildChatRequestBody = (messageHistory: Message[]) => ({
  model: selectedModel,
  messages: messageHistory,
  stream: true,
  ...buildChatRequestOverrides(appSettings),
});

const sendMessage = async () => {
    if ((!inputValue.trim() && uploadedImages.length === 0) || isLoading) return;

    // Cancel any existing request
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }

    // Create new abort controller
    abortControllerRef.current = new AbortController();
    
    // When sending a new message, ensure we're at the bottom and reset scroll tracking
    setIsUserAtBottom(true);
    userScrolledAwayRef.current = false;

    // Check if the selected model is different from the currently loaded model
    // If so, show the model loading indicator
    const needsModelLoad = currentLoadedModel !== selectedModel;
    if (needsModelLoad) {
      setIsModelLoading(true);
    }

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
    let receivedFirstChunk = false;

    try {
      const response = await serverFetch('/chat/completions', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(buildChatRequestBody(messageHistory)),
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
                  // First response received - model is loaded, clear loading indicator
                  if (!receivedFirstChunk) {
                    receivedFirstChunk = true;
                    setIsModelLoading(false);
                    setCurrentLoadedModel(selectedModel);
                  }
                  
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
      setIsModelLoading(false); // Clear loading state on error or completion
      abortControllerRef.current = null;
      // Reset scroll tracking after streaming ends so next message can autoscroll
      userScrolledAwayRef.current = false;
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

  const adjustTextareaHeight = (textarea: HTMLTextAreaElement) => {
    // Reset height to auto to get the correct scrollHeight
    textarea.style.height = 'auto';
    const maxHeight = 200;
    const newHeight = Math.min(textarea.scrollHeight, maxHeight);
    textarea.style.height = newHeight + 'px';
    // Show scrollbar only when content exceeds max height
    textarea.style.overflowY = textarea.scrollHeight > maxHeight ? 'auto' : 'hidden';
  };

  // Reset textarea height when input is cleared (after sending)
  useEffect(() => {
    if (inputTextareaRef.current && inputValue === '') {
      inputTextareaRef.current.style.height = 'auto';
      inputTextareaRef.current.style.overflowY = 'hidden';
    }
  }, [inputValue]);

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
    
    // When submitting an edit, ensure we're at the bottom and reset scroll tracking
    setIsUserAtBottom(true);
    userScrolledAwayRef.current = false;

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

    // Check if the selected model is different from the currently loaded model
    const needsModelLoad = currentLoadedModel !== selectedModel;
    if (needsModelLoad) {
      setIsModelLoading(true);
    }

    // Add placeholder for assistant message
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    let accumulatedContent = '';
    let accumulatedThinking = '';
    let receivedFirstChunk = false;

    try {
      const response = await serverFetch('/chat/completions', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(buildChatRequestBody(messageHistory)),
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
                  // First response received - model is loaded, clear loading indicator
                  if (!receivedFirstChunk) {
                    receivedFirstChunk = true;
                    setIsModelLoading(false);
                    setCurrentLoadedModel(selectedModel);
                  }
                  
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
      setIsModelLoading(false); // Clear loading state on error or completion
      abortControllerRef.current = null;
      // Reset scroll tracking after streaming ends so next message can autoscroll
      userScrolledAwayRef.current = false;
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
    userScrolledAwayRef.current = false;
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
              d="M21 3V8M21 8H16M21 8L18 5.29168C16.4077 3.86656 14.3051 3 12 3C7.02944 3 3 7.02944 3 12C3 16.9706 7.02944 21 12 21C16.2832 21 19.8675 18.008 20.777 14"
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
                            transform="translate(-1, 1)"
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
        {isLoading && isModelLoading && (
          <div className="model-loading-indicator">
            <span className="model-loading-text">Loading model</span>
          </div>
        )}
        {isLoading && !isModelLoading && (
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
            onChange={handleInputChange}
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
                onChange={(e) => {
                  userHasSelectedModelRef.current = true;
                  setSelectedModel(e.target.value);
                }}
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
                    transform="translate(-1, 1)"
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


