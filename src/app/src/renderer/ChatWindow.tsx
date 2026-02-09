import React, { useState, useEffect, useRef } from 'react';
import MarkdownMessage from './MarkdownMessage';
// @ts-ignore - SVG assets live outside of the TypeScript rootDir for Electron packaging
import logoSvg from '../../assets/logo.svg';
import {
  AppSettings,
  buildChatRequestOverrides,
  mergeWithDefaultSettings,
} from './utils/appSettings';
import { serverFetch } from './utils/serverConfig';
import { downloadTracker } from './utils/downloadTracker';
import { useModels, DEFAULT_MODEL_ID } from './hooks/useModels';

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

interface ChatWindowProps {
  isVisible: boolean;
  width?: number;
}

const ChatWindow: React.FC<ChatWindowProps> = ({ isVisible, width }) => {
  // Get shared model data from context
  const {
    modelsData,
    downloadedModels,
    selectedModel,
    setSelectedModel,
    isDefaultModelPending,
    refresh: refreshModels,
    userHasSelectedModel,
    setUserHasSelectedModel,
  } = useModels();

  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [isLoading, setIsLoading] = useState(false);
  const [currentLoadedModel, setCurrentLoadedModel] = useState<string | null>(null);
  const [isModelLoading, setIsModelLoading] = useState(false);
  // Track if we're downloading a model for a pending message (first-time user experience)
  const [isDownloadingForChat, setIsDownloadingForChat] = useState(false);
  // Store pending message content to send after download completes
  const pendingMessageRef = useRef<{ content: MessageContent; images: string[] } | null>(null);
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

  // Embedding model state
  const [embeddingInput, setEmbeddingInput] = useState('');
  const [embeddingHistory, setEmbeddingHistory] = useState<Array<{input: string, embedding: number[], dimensions?: number}>>([]);
  const [isProcessingEmbedding, setIsProcessingEmbedding] = useState(false);
  const [expandedEmbeddings, setExpandedEmbeddings] = useState<Set<number>>(new Set());

  // Reranking model state
  const [rerankQuery, setRerankQuery] = useState('');
  const [rerankDocuments, setRerankDocuments] = useState('');
  const [rerankHistory, setRerankHistory] = useState<Array<{
    query: string;
    documents: string;
    results: Array<{index: number, text: string, score: number}>;
  }>>([]);
  const [isProcessingRerank, setIsProcessingRerank] = useState(false);

  // Transcription model state
  const [transcriptionFile, setTranscriptionFile] = useState<File | null>(null);
  const [transcriptionHistory, setTranscriptionHistory] = useState<Array<{
    filename: string;
    text: string;
  }>>([]);
  const [isProcessingTranscription, setIsProcessingTranscription] = useState(false);
  const audioFileInputRef = useRef<HTMLInputElement>(null);

  // Image generation model state
  const [imagePrompt, setImagePrompt] = useState('');
  const [imageHistory, setImageHistory] = useState<Array<{
    prompt: string;
    imageData: string;  // base64 data
    timestamp: number;
  }>>([]);
  const [isGeneratingImage, setIsGeneratingImage] = useState(false);

  // Image generation settings
  interface ImageSettings {
    steps: number;
    cfgScale: number;
    width: number;
    height: number;
    seed: number;
  }

  const DEFAULT_IMAGE_SETTINGS: ImageSettings = {
    steps: 20,
    cfgScale: 7.0,
    width: 512,
    height: 512,
    seed: -1,
  };

  const [imageSettings, setImageSettings] = useState<ImageSettings>(DEFAULT_IMAGE_SETTINGS);

useEffect(() => {
  fetchLoadedModel();
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
      setUserHasSelectedModel(false);
    } else {
      // Fallback: fetch the loaded model from the health endpoint
      fetchLoadedModel();
    }
  };

  const handleModelUnload = () => {
    // Model was unloaded/ejected - reset the current loaded model state
    setCurrentLoadedModel(null);
  };

  const handleModelLoadStart = (e: CustomEvent) => {
    setSelectedModel(e.detail.modelId);
  }

  window.addEventListener('modelLoadStart' as any, handleModelLoadStart);
  window.addEventListener('modelLoadEnd' as any, handleModelLoadEnd);
  window.addEventListener('modelUnload' as any, handleModelUnload);

  // Periodically check health status to detect when another app unloads the model
  const healthCheckInterval = setInterval(() => {
    fetchLoadedModel();
  }, 5000);

  return () => {
    window.removeEventListener('modelLoadStart' as any, handleModelLoadStart);
    window.removeEventListener('modelLoadEnd' as any, handleModelLoadEnd);
    window.removeEventListener('modelUnload' as any, handleModelUnload);
    clearInterval(healthCheckInterval);
    if (typeof unsubscribeSettings === 'function') {
      unsubscribeSettings();
    }
  };
}, [setSelectedModel, setUserHasSelectedModel]);

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

  // Load model-specific image defaults when the selected model changes
  useEffect(() => {
    if (isImageGenerationModel() && selectedModel) {
      const modelInfo = modelsData[selectedModel];
      const defaults = modelInfo?.image_defaults;
      setImageSettings({
        steps: defaults?.steps ?? DEFAULT_IMAGE_SETTINGS.steps,
        cfgScale: defaults?.cfg_scale ?? DEFAULT_IMAGE_SETTINGS.cfgScale,
        width: defaults?.width ?? DEFAULT_IMAGE_SETTINGS.width,
        height: defaults?.height ?? DEFAULT_IMAGE_SETTINGS.height,
        seed: -1,  // Always reset seed to random
      });
    }
  }, [selectedModel, modelsData]);

  // Auto-scroll to bottom when new images are generated
  useEffect(() => {
    if (imageHistory.length > 0) {
      requestAnimationFrame(() => {
        messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
      });
    }
  }, [imageHistory.length]);

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

const fetchLoadedModel = async () => {
  try {
    const response = await serverFetch('/health');
    const data = await response.json();

    if (data?.model_loaded) {
      setCurrentLoadedModel(data.model_loaded);
      // Only auto-select if user hasn't manually chosen a model
      if (!userHasSelectedModel) {
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

  const isVisionModel = (): boolean => {
    if (!selectedModel) return false;

    const modelInfo = modelsData[selectedModel];

    return modelInfo?.labels?.includes('vision') || false;
  };

  const isEmbeddingModel = (): boolean => {
    if (!selectedModel) return false;

    const modelInfo = modelsData[selectedModel];

    return !!(modelInfo?.labels?.includes('embeddings') || (modelInfo as any)?.embedding);
  };

  const isRerankingModel = (): boolean => {
    if (!selectedModel) return false;

    const modelInfo = modelsData[selectedModel];

    return !!(modelInfo?.labels?.includes('reranking') || (modelInfo as any)?.reranking);
  };

  const isTranscriptionModel = (): boolean => {
    if (!selectedModel) return false;

    const modelInfo = modelsData[selectedModel];

    return modelInfo?.recipe === 'whispercpp';
  };

  const isImageGenerationModel = (): boolean => {
    if (!selectedModel) return false;

    const modelInfo = modelsData[selectedModel];

    // Debug logging
    console.log('[ImageDetection] selectedModel:', selectedModel);
    console.log('[ImageDetection] modelInfo:', modelInfo);
    console.log('[ImageDetection] recipe:', modelInfo?.recipe);
    console.log('[ImageDetection] labels:', modelInfo?.labels);

    return modelInfo?.recipe === 'sd-cpp' ||
           modelInfo?.labels?.includes('image') || false;
  };

  const getModelType = (): 'llm' | 'embedding' | 'reranking' | 'transcription' | 'image' => {
    if (isEmbeddingModel()) return 'embedding';
    if (isRerankingModel()) return 'reranking';
    if (isTranscriptionModel()) return 'transcription';
    if (isImageGenerationModel()) return 'image';
    return 'llm';
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

// Helper function to extract thinking from content with <think> tags
const extractThinking = (content: string): { content: string; thinking: string } => {
  let extractedThinking = '';
  let cleanedContent = content;

  // Extract all complete <think>...</think> blocks
  const thinkRegex = /<think>([\s\S]*?)<\/think>/g;
  let match;

  while ((match = thinkRegex.exec(content)) !== null) {
    extractedThinking += match[1];
  }

  // Remove all complete <think>...</think> blocks from content
  cleanedContent = cleanedContent.replace(thinkRegex, '');

  return {
    content: cleanedContent,
    thinking: extractedThinking
  };
};

// Shared streaming handler for both new messages and edits
const handleStreamingResponse = async (messageHistory: Message[]): Promise<void> => {
  let accumulatedContent = '';
  let accumulatedThinking = '';
  let receivedFirstChunk = false;

  const response = await serverFetch('/chat/completions', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(buildChatRequestBody(messageHistory)),
    signal: abortControllerRef.current!.signal,
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

              // Extract thinking from <think> tags in content
              const extracted = extractThinking(accumulatedContent);
              const displayContent = extracted.content;
              const embeddedThinking = extracted.thinking;

              // Combine thinking from both sources (API field and embedded tags)
              const totalThinking = (accumulatedThinking || '') + (embeddedThinking || '');

              setMessages(prev => {
                const newMessages = [...prev];
                const messageIndex = newMessages.length - 1;
                newMessages[messageIndex] = {
                  role: 'assistant',
                  content: displayContent,
                  thinking: totalThinking || undefined,
                };

                // Auto-expand thinking section if thinking content is present
                // and collapseThinkingByDefault is not enabled
                if (totalThinking && !appSettings?.collapseThinkingByDefault?.value) {
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
};

/**
 * Download a model with SSE progress tracking.
 * Used for first-time user experience when no models are downloaded.
 */
const downloadModelForChat = async (modelName: string): Promise<boolean> => {
  return new Promise((resolve) => {
    const abortController = new AbortController();
    const downloadId = downloadTracker.startDownload(modelName, abortController);

    // Dispatch event to open download manager
    window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName } }));

    let downloadCompleted = false;

    const performDownload = async () => {
      try {
        const response = await serverFetch('/pull', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ model_name: modelName, stream: true }),
          signal: abortController.signal,
        });

        if (!response.ok) {
          throw new Error(`Failed to download model: ${response.statusText}`);
        }

        const reader = response.body?.getReader();
        if (!reader) {
          throw new Error('No response body');
        }

        const decoder = new TextDecoder();
        let buffer = '';
        let currentEventType = 'progress';

        while (true) {
          const { done, value } = await reader.read();

          if (done) break;

          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split('\n');
          buffer = lines.pop() || '';

          for (const line of lines) {
            if (line.startsWith('event:')) {
              currentEventType = line.substring(6).trim();
            } else if (line.startsWith('data:')) {
              try {
                const data = JSON.parse(line.substring(5).trim());

                if (currentEventType === 'progress') {
                  downloadTracker.updateProgress(downloadId, data);
                } else if (currentEventType === 'complete') {
                  downloadTracker.completeDownload(downloadId);
                  downloadCompleted = true;
                } else if (currentEventType === 'error') {
                  downloadTracker.failDownload(downloadId, data.error || 'Unknown error');
                  throw new Error(data.error || 'Download failed');
                }
              } catch (parseError) {
                console.error('Failed to parse SSE data:', line, parseError);
              }
            } else if (line.trim() === '') {
              currentEventType = 'progress';
            }
          }
        }

        if (!downloadCompleted) {
          downloadTracker.completeDownload(downloadId);
          downloadCompleted = true;
        }

        // Notify all components that models have been updated
        // (The ModelsProvider listens for this and refreshes automatically)
        window.dispatchEvent(new CustomEvent('modelsUpdated'));

        resolve(true);
      } catch (error: any) {
        if (error.name === 'AbortError') {
          downloadTracker.cancelDownload(downloadId);
        } else {
          downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
          console.error('Error downloading model:', error);
        }
        resolve(false);
      }
    };

    performDownload();
  });
};

const sendMessage = async () => {
    if ((!inputValue.trim() && uploadedImages.length === 0) || isLoading || isDownloadingForChat) return;

    // Cancel any existing request
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }

    // Create new abort controller
    abortControllerRef.current = new AbortController();

    // When sending a new message, ensure we're at the bottom and reset scroll tracking
    setIsUserAtBottom(true);
    userScrolledAwayRef.current = false;

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

    // If the default model is pending (not yet downloaded), we need to download it first
    if (isDefaultModelPending && selectedModel === DEFAULT_MODEL_ID) {
      // Store the pending message
      pendingMessageRef.current = { content: messageContent, images: [...uploadedImages] };
      setInputValue('');
      setUploadedImages([]);
      setIsDownloadingForChat(true);

      // Show the user message immediately
      const userMessage: Message = { role: 'user', content: messageContent };
      setMessages(prev => [...prev, userMessage]);

      // Download the model
      const downloadSuccess = await downloadModelForChat(selectedModel);

      if (downloadSuccess) {
        // Model downloaded successfully - close download manager
        window.dispatchEvent(new CustomEvent('download:chatComplete'));

        // Now send the message (isDefaultModelPending will be updated by the hook)
        const pendingMessage = pendingMessageRef.current;
        pendingMessageRef.current = null;

        if (pendingMessage) {
          // Continue with the chat request
          setIsLoading(true);
          setIsModelLoading(true);

          // Add placeholder for assistant message
          setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

          try {
            const messageHistory = messages.concat([{ role: 'user' as const, content: pendingMessage.content }]);
            await handleStreamingResponse(messageHistory);
          } catch (error: any) {
            if (error.name === 'AbortError') {
              console.log('Request aborted - keeping partial response');
              setMessages(prev => {
                const lastMessage = prev[prev.length - 1];
                if (!lastMessage || (!lastMessage.content && !lastMessage.thinking)) {
                  return prev.slice(0, -1);
                }
                return prev;
              });
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
            setIsModelLoading(false);
            abortControllerRef.current = null;
            userScrolledAwayRef.current = false;
          }
        }
      } else {
        // Download failed - show error
        setMessages(prev => [...prev, {
          role: 'assistant',
          content: 'Failed to download the model. Please try again or download a model from the Model Manager.'
        }]);
        pendingMessageRef.current = null;
      }

      setIsDownloadingForChat(false);
      return;
    }

    // Normal flow - model is already downloaded
    const userMessage: Message = { role: 'user', content: messageContent };
    const messageHistory = [...messages, userMessage];

    setMessages(prev => [...prev, userMessage]);
    setInputValue('');
    setUploadedImages([]);
    setIsLoading(true);

    // Check if the selected model is different from the currently loaded model
    // If so, show the model loading indicator
    const needsModelLoad = currentLoadedModel !== selectedModel;
    if (needsModelLoad) {
      setIsModelLoading(true);
    }

    // Add placeholder for assistant message
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    try {
      await handleStreamingResponse(messageHistory);
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        // Keep the partial message that was received
        // If no content was received, remove the empty message
        setMessages(prev => {
          const lastMessage = prev[prev.length - 1];
          if (!lastMessage || (!lastMessage.content && !lastMessage.thinking)) {
            return prev.slice(0, -1);
          }
          return prev;
        });
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
      // Notify StatusBar to refresh server stats
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

  const renderMessageContent = (content: MessageContent, thinking?: string, messageIndex?: number, isComplete?: boolean) => {
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
              if (item.type === 'text') {
                return <MarkdownMessage key={index} content={item.text} isComplete={isComplete} />;
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

    try {
      await handleStreamingResponse(messageHistory);
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        // Keep the partial message that was received
        // If no content was received, remove the empty message
        setMessages(prev => {
          const lastMessage = prev[prev.length - 1];
          if (!lastMessage || (!lastMessage.content && !lastMessage.thinking)) {
            return prev.slice(0, -1);
          }
          return prev;
        });
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
      // Notify StatusBar to refresh server stats
      window.dispatchEvent(new CustomEvent('inference-complete'));
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

    // Clear embedding/reranking state
    setEmbeddingInput('');
    setEmbeddingHistory([]);
    setRerankQuery('');
    setRerankDocuments('');
    setRerankHistory([]);

    // Clear image generation state
    setImagePrompt('');
    setImageHistory([]);
    setIsGeneratingImage(false);

    // Clear transcription state
    setTranscriptionFile(null);
    setTranscriptionHistory([]);
    setIsProcessingTranscription(false);
    if (audioFileInputRef.current) {
      audioFileInputRef.current.value = '';
    }
  };

  const handleEmbedding = async () => {
    if (!embeddingInput.trim() || isProcessingEmbedding) return;

    const currentInput = embeddingInput;
    setIsProcessingEmbedding(true);
    setEmbeddingInput(''); // Clear input after submitting

    try {
      const requestBody: any = {
        model: selectedModel,
        input: currentInput
      };

      const response = await serverFetch('/embeddings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(requestBody)
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      const data = await response.json();

      // Extract the embedding from the response
      // OpenAI format: { data: [{ embedding: [...] }] }
      let embedding: number[];
      if (data.data && data.data[0] && data.data[0].embedding) {
        embedding = data.data[0].embedding;
      } else if (Array.isArray(data)) {
        embedding = data;
      } else {
        throw new Error('Unexpected response format');
      }

      // Add to history
      setEmbeddingHistory(prev => [...prev, {
        input: currentInput,
        embedding
      }]);
    } catch (error: any) {
      console.error('Failed to get embedding:', error);
      alert(`Failed to get embedding: ${error.message || 'Unknown error'}`);
    } finally {
      setIsProcessingEmbedding(false);
    }
  };

  const toggleEmbeddingExpansion = (index: number) => {
    setExpandedEmbeddings(prev => {
      const next = new Set(prev);
      if (next.has(index)) {
        next.delete(index);
      } else {
        next.add(index);
      }
      return next;
    });
  };

  const handleReranking = async () => {
    if (!rerankQuery.trim() || !rerankDocuments.trim() || isProcessingRerank) return;

    const currentQuery = rerankQuery;
    const currentDocuments = rerankDocuments;

    setIsProcessingRerank(true);

    try {
      // Parse documents - assume one document per line
      const docs = currentDocuments.split('\n').filter(d => d.trim());

      const response = await serverFetch('/reranking', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model: selectedModel,
          query: currentQuery,
          documents: docs
        })
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      const data = await response.json();

      // Extract the reranking results from the response
      // Expected format: { results: [{ index: 0, relevance_score: 0.95 }] }
      if (data.results && Array.isArray(data.results)) {
        const results = data.results.map((r: any) => ({
          index: r.index,
          text: docs[r.index],
          score: r.relevance_score || r.score || 0
        }));

        // Add to history
        setRerankHistory(prev => [...prev, {
          query: currentQuery,
          documents: currentDocuments,
          results
        }]);
      } else {
        throw new Error('Unexpected response format');
      }
    } catch (error: any) {
      console.error('Failed to rerank:', error);
      alert(`Failed to rerank: ${error.message || 'Unknown error'}`);
    } finally {
      setIsProcessingRerank(false);
    }
  };

  const handleAudioFileSelect = (event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;
    if (!files || files.length === 0) return;

    const file = files[0];
    setTranscriptionFile(file);
  };

  const handleTranscription = async () => {
    if (!transcriptionFile || isProcessingTranscription) return;

    const currentFile = transcriptionFile;
    setIsProcessingTranscription(true);

    try {
      // Create FormData for multipart/form-data request
      const formData = new FormData();
      formData.append('file', currentFile);
      formData.append('model', selectedModel);

      const response = await serverFetch('/audio/transcriptions', {
        method: 'POST',
        body: formData
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      const data = await response.json();

      // Extract the transcription text from the response
      let transcriptionText: string;
      if (data.text) {
        transcriptionText = data.text;
      } else {
        throw new Error('Unexpected response format');
      }

      // Add to history
      setTranscriptionHistory(prev => [...prev, {
        filename: currentFile.name,
        text: transcriptionText
      }]);

      // Clear the file input
      setTranscriptionFile(null);
      if (audioFileInputRef.current) {
        audioFileInputRef.current.value = '';
      }

    } catch (error: any) {
      console.error('Failed to transcribe:', error);
      alert(`Failed to transcribe: ${error.message || 'Unknown error'}`);
    } finally {
      setIsProcessingTranscription(false);
    }
  };

  const handleImageGeneration = async () => {
    if (!imagePrompt.trim() || isGeneratingImage) return;

    const currentPrompt = imagePrompt;
    setIsGeneratingImage(true);
    setImagePrompt(''); // Clear input immediately

    try {
      // Build request body with current settings
      const requestBody: Record<string, unknown> = {
        model: selectedModel,
        prompt: currentPrompt,
        size: `${imageSettings.width}x${imageSettings.height}`,
        steps: imageSettings.steps,
        cfg_scale: imageSettings.cfgScale,
        response_format: 'b64_json'
      };

      // Only include seed if it's positive (otherwise random)
      if (imageSettings.seed > 0) {
        requestBody.seed = imageSettings.seed;
      }

      const response = await serverFetch('/images/generations', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(requestBody)
      });

      if (!response.ok) {
        const errorData = await response.json();
        throw new Error(errorData.error?.message || `HTTP error! status: ${response.status}`);
      }

      const data = await response.json();

      if (data.data && data.data[0] && data.data[0].b64_json) {
        setImageHistory(prev => [...prev, {
          prompt: currentPrompt,
          imageData: data.data[0].b64_json,
          timestamp: Date.now()
        }]);
      } else {
        throw new Error('Unexpected response format');
      }

    } catch (error: any) {
      console.error('Failed to generate image:', error);
      alert(`Failed to generate image: ${error.message || 'Unknown error'}`);
    } finally {
      setIsGeneratingImage(false);
    }
  };

  const saveGeneratedImage = (imageData: string, prompt: string) => {
    const link = document.createElement('a');
    link.href = `data:image/png;base64,${imageData}`;
    // Create filename from prompt (sanitized, max 30 chars)
    const sanitizedPrompt = prompt.slice(0, 30).replace(/[^a-z0-9]/gi, '_');
    const filename = `lemonade_${sanitizedPrompt}_${Date.now()}.png`;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  };

  if (!isVisible) return null;

  const modelType = getModelType();
  const headerTitle = modelType === 'embedding' ? 'Lemonade Embeddings'
                    : modelType === 'reranking' ? 'Lemonade Reranking'
                    : modelType === 'transcription' ? 'Lemonade Transcriber'
                    : modelType === 'image' ? 'Lemonade Image Generator'
                    : 'LLM Chat';

  // Reusable components
  const EmptyState = ({ title }: { title: string }) => (
    <div className="chat-empty-state">
      <img src={logoSvg} alt="Lemonade Logo" className="chat-empty-logo" />
      <h2 className="chat-empty-title">{title}</h2>
    </div>
  );

  const TypingIndicator = ({ size = 'normal' }: { size?: 'normal' | 'small' }) => (
    <div className={`typing-indicator${size === 'small' ? ' small' : ''}`}>
      <span></span>
      <span></span>
      <span></span>
    </div>
  );

  // Build the list of models for the dropdown
  const dropdownModels = isDefaultModelPending
    ? [{ id: DEFAULT_MODEL_ID }]
    : downloadedModels;

  const ModelSelector = ({ disabled }: { disabled: boolean }) => (
    <select
      className="model-selector"
      value={selectedModel}
      onChange={(e) => {
        setUserHasSelectedModel(true);
        setSelectedModel(e.target.value);
      }}
      disabled={disabled}
    >
      {dropdownModels.map((model) => (
        <option key={model.id} value={model.id}>
          {model.id}
        </option>
      ))}
    </select>
  );

  const SendButton = ({ onClick, disabled }: { onClick: () => void; disabled: boolean }) => (
    <button
      className="chat-send-button"
      onClick={onClick}
      disabled={disabled}
      title="Send"
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
  );

  return (
    <div className="chat-window" style={width ? { width: `${width}px` } : undefined}>
      <div className="chat-header">
        <h3>{headerTitle}</h3>
        <button
          className="new-chat-button"
          onClick={handleNewChat}
          disabled={isLoading || isProcessingEmbedding || isProcessingRerank || isDownloadingForChat}
          title={modelType === 'llm' ? 'Start a new chat' : 'Clear'}
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

      {/* Embedding Model UI */}
      {modelType === 'embedding' && (
        <>
          <div className="chat-messages" ref={messagesContainerRef}>
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
                    <button
                      className="embedding-toggle-button"
                      onClick={() => toggleEmbeddingExpansion(index)}
                    >
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

            {isProcessingEmbedding && (
              <div className="chat-message assistant-message">
                <TypingIndicator />
              </div>
            )}
            <div ref={messagesEndRef} />
          </div>

          <div className="chat-input-container">
            <div className="chat-input-wrapper">
              <textarea
                ref={inputTextareaRef}
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
                disabled={isProcessingEmbedding}
              />
              <div className="chat-controls">
                <div className="chat-controls-left">
                  <ModelSelector disabled={isProcessingEmbedding} />
                </div>
                <SendButton onClick={handleEmbedding} disabled={!embeddingInput.trim() || isProcessingEmbedding} />
              </div>
            </div>
          </div>
        </>
      )}

      {/* Reranking Model UI */}
      {modelType === 'reranking' && (
        <>
          <div className="chat-messages" ref={messagesContainerRef}>
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

            {isProcessingRerank && (
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
                  disabled={isProcessingRerank}
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
                  disabled={isProcessingRerank}
                />
              </div>

              <div className="chat-controls">
                <div className="chat-controls-left">
                  <ModelSelector disabled={isProcessingRerank} />
                </div>
                <button
                  className="chat-send-button"
                  onClick={handleReranking}
                  disabled={!rerankQuery.trim() || !rerankDocuments.trim() || isProcessingRerank}
                  title="Rerank documents"
                >
                  {isProcessingRerank ? (
                    <TypingIndicator size="small" />
                  ) : (
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                      <path
                        d="M3 8L6 5L9 8M6 5V19M21 16L18 19L15 16M18 19V5"
                        stroke="currentColor"
                        strokeWidth="2"
                        strokeLinecap="round"
                        strokeLinejoin="round"
                      />
                    </svg>
                  )}
                </button>
              </div>
            </div>
          </div>
        </>
      )}

      {/* Transcription Model UI */}
      {modelType === 'transcription' && (
        <>
          <div className="chat-messages" ref={messagesContainerRef}>
            {transcriptionHistory.length === 0 && <EmptyState title="Lemonade Transcriber" />}

            {transcriptionHistory.map((item, index) => (
              <div key={index} className="transcription-history-item">
                <div className="transcription-file-info">
                  <div className="transcription-label">
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" style={{ marginRight: '8px' }}>
                      <path
                        d="M12 15V3M12 15L8 11M12 15L16 11M2 17L2.621 19.485C2.725 19.871 2.777 20.064 2.873 20.213C2.958 20.345 3.073 20.454 3.209 20.531C3.364 20.618 3.558 20.658 3.947 20.737L11.053 22.147C11.442 22.226 11.636 22.266 11.791 22.179C11.927 22.102 12.042 21.993 12.127 21.861C12.223 21.712 12.275 21.519 12.379 21.133L13 18.5M22 17L21.379 19.485C21.275 19.871 21.223 20.064 21.127 20.213C21.042 20.345 20.927 20.454 20.791 20.531C20.636 20.618 20.442 20.658 20.053 20.737L12.947 22.147C12.558 22.226 12.364 22.266 12.209 22.179C12.073 22.102 11.958 21.993 11.873 21.861C11.777 21.712 11.725 21.519 11.621 21.133L11 18.5"
                        stroke="currentColor"
                        strokeWidth="2"
                        strokeLinecap="round"
                        strokeLinejoin="round"
                      />
                    </svg>
                    {item.filename}
                  </div>
                </div>

                <div className="transcription-result-container">
                  <div className="transcription-result-header">
                    <h4>Transcription</h4>
                  </div>
                  <div className="transcription-result">
                    {item.text}
                  </div>
                </div>
              </div>
            ))}

            {isProcessingTranscription && (
              <div className="chat-message assistant-message">
                <TypingIndicator />
              </div>
            )}
            <div ref={messagesEndRef} />
          </div>

          <div className="chat-input-container">
            <div className="chat-input-wrapper">
              <input
                ref={audioFileInputRef}
                type="file"
                accept="audio/*"
                onChange={handleAudioFileSelect}
                style={{ display: 'none' }}
              />

              <div className="transcription-file-display">
                {transcriptionFile ? (
                  <div className="transcription-file-info-display">
                    <span className="file-name">{transcriptionFile.name}</span>
                    <span className="file-size-indicator">
                      {(transcriptionFile.size / 1024 / 1024).toFixed(2)} MB
                    </span>
                  </div>
                ) : (
                  <span className="transcription-placeholder">No audio file selected</span>
                )}
              </div>

              <div className="chat-controls">
                <div className="chat-controls-left">
                  <button
                    className="audio-file-button"
                    onClick={() => audioFileInputRef.current?.click()}
                    disabled={isProcessingTranscription}
                    title="Choose audio file"
                  >
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                      <path
                        d="M21 15V19C21 20.1 20.1 21 19 21H5C3.9 21 3 20.1 3 19V15M17 8L12 3M12 3L7 8M12 3V15"
                        stroke="currentColor"
                        strokeWidth="2"
                        strokeLinecap="round"
                        strokeLinejoin="round"
                      />
                    </svg>
                  </button>
                  <ModelSelector disabled={isProcessingTranscription} />
                </div>
                <SendButton
                  onClick={handleTranscription}
                  disabled={!transcriptionFile || isProcessingTranscription}
                />
              </div>
            </div>
          </div>
        </>
      )}

      {/* Image Generation UI */}
      {modelType === 'image' && (
        <>
          <div className="chat-messages" ref={messagesContainerRef}>
            {imageHistory.length === 0 && (
              <EmptyState title="Lemonade Image Generator" />
            )}

            {imageHistory.map((item, index) => (
              <div key={index} className="image-generation-item">
                <div className="image-prompt-display">
                  <span className="prompt-label">Prompt:</span>
                  <span className="prompt-text">{item.prompt}</span>
                </div>

                <div className="generated-image-container">
                  <img
                    src={`data:image/png;base64,${item.imageData}`}
                    alt={item.prompt}
                    className="generated-image"
                  />
                  <button
                    className="save-image-button"
                    onClick={() => saveGeneratedImage(item.imageData, item.prompt)}
                  >
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
                      <polyline points="7 10 12 15 17 10"/>
                      <line x1="12" y1="15" x2="12" y2="3"/>
                    </svg>
                    Save Image
                  </button>
                </div>
              </div>
            ))}

            {isGeneratingImage && (
              <div className="image-generating-indicator">
                <div className="generating-spinner"></div>
                <span>Generating image...</span>
              </div>
            )}
            <div ref={messagesEndRef} />
          </div>

          {/* Image Settings Panel */}
          <div className="image-settings-panel">
            <div className="image-setting">
              <label>Steps</label>
              <input
                type="number"
                min="1"
                max="50"
                value={imageSettings.steps}
                onChange={(e) => setImageSettings(prev => ({ ...prev, steps: parseInt(e.target.value) || 1 }))}
                disabled={isGeneratingImage}
              />
            </div>
            <div className="image-setting">
              <label>CFG Scale</label>
              <input
                type="number"
                min="1"
                max="20"
                step="0.5"
                value={imageSettings.cfgScale}
                onChange={(e) => setImageSettings(prev => ({ ...prev, cfgScale: parseFloat(e.target.value) || 1 }))}
                disabled={isGeneratingImage}
              />
            </div>
            <div className="image-setting">
              <label>Width</label>
              <select
                value={imageSettings.width}
                onChange={(e) => setImageSettings(prev => ({ ...prev, width: parseInt(e.target.value) }))}
                disabled={isGeneratingImage}
              >
                <option value="512">512</option>
                <option value="768">768</option>
                <option value="1024">1024</option>
              </select>
            </div>
            <div className="image-setting">
              <label>Height</label>
              <select
                value={imageSettings.height}
                onChange={(e) => setImageSettings(prev => ({ ...prev, height: parseInt(e.target.value) }))}
                disabled={isGeneratingImage}
              >
                <option value="512">512</option>
                <option value="768">768</option>
                <option value="1024">1024</option>
              </select>
            </div>
            <div className="image-setting">
              <label>Seed</label>
              <input
                type="number"
                min="-1"
                value={imageSettings.seed}
                onChange={(e) => setImageSettings(prev => ({ ...prev, seed: parseInt(e.target.value) || -1 }))}
                disabled={isGeneratingImage}
                placeholder="-1 = random"
              />
            </div>
          </div>

          <div className="chat-input-container">
            <div className="chat-input-wrapper">
              <textarea
                ref={inputTextareaRef}
                className="chat-input"
                value={imagePrompt}
                onChange={(e) => {
                  setImagePrompt(e.target.value);
                  adjustTextareaHeight(e.target);
                }}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' && !e.shiftKey) {
                    e.preventDefault();
                    handleImageGeneration();
                  }
                }}
                placeholder="Describe the image you want to generate..."
                rows={1}
                disabled={isGeneratingImage}
              />
              <div className="chat-controls">
                <div className="chat-controls-left">
                  <ModelSelector disabled={isGeneratingImage} />
                </div>
                <button
                  className="chat-send-button"
                  onClick={handleImageGeneration}
                  disabled={!imagePrompt.trim() || isGeneratingImage}
                  title="Generate"
                >
                  {isGeneratingImage ? (
                    <TypingIndicator size="small" />
                  ) : (
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
                  )}
                </button>
              </div>
            </div>
          </div>
        </>
      )}

      {/* LLM Chat UI (default) */}
      {modelType === 'llm' && (
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
                      {renderMessageContent(message.content, message.thinking, index, message.role === 'assistant')}
                    </div>
                  )}
                </div>
              );
            })}
            {isDownloadingForChat && (
              <div className="model-loading-indicator">
                <span className="model-loading-text">Downloading model...</span>
              </div>
            )}
            {isLoading && isModelLoading && !isDownloadingForChat && (
              <div className="model-loading-indicator">
                <span className="model-loading-text">Loading model</span>
              </div>
            )}
            {isLoading && !isModelLoading && !isDownloadingForChat && (
              <div className="chat-message assistant-message">
                <TypingIndicator />
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
                placeholder={isDownloadingForChat ? "Downloading model..." : "Type your message..."}
                rows={1}
                disabled={isLoading || isDownloadingForChat}
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
                        disabled={isLoading || isDownloadingForChat}
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
                  <ModelSelector disabled={isLoading || isDownloadingForChat} />
                </div>
                {isLoading || isDownloadingForChat ? (
                  <button
                    className="chat-stop-button"
                    onClick={handleStopGeneration}
                    title="Stop generation"
                    disabled={isDownloadingForChat}
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
                  <SendButton onClick={sendMessage} disabled={!inputValue.trim() && uploadedImages.length === 0} />
                )}
              </div>
            </div>
          </div>
        </>
      )}
    </div>
  );
};

export default ChatWindow;
