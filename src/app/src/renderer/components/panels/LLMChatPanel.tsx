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
import { Message, MessageContent, TextContent, ImageContent, AudioContent, Artifact } from '../../utils/chatTypes';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import { SendIcon, ImageUploadIcon, RefreshIcon } from '../Icons';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import ImagePreviewList from '../ImagePreviewList';
import EmptyState from '../EmptyState';
import TypingIndicator from '../TypingIndicator';
import { getExperiencePrimaryChatModel } from '../../utils/experienceModels';
import RecordButton from '../RecordButton';
import {
  buildLemonadeTools,
  executeLemonadeTool,
  LemonadeToolsResult,
  ToolExecutionContext,
} from '../../utils/lemonadeTools';

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
  experienceMode?: boolean;
  currentLoadedModel: string | null;
  setCurrentLoadedModel: React.Dispatch<React.SetStateAction<string | null>>;
  onNewChat?: () => void;
}

const LLMChatPanel: React.FC<LLMChatPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality,
  runPreFlight, reset, showError, appSettings,
  isVision, experienceMode = false, currentLoadedModel, setCurrentLoadedModel,
  onNewChat,
}) => {
  const { selectedModel, modelsData } = useModels();
  const { systemInfo } = useSystem();
  const tts = useTTS(appSettings, modelsData);
  const chatModelName = useMemo(
    () => getExperiencePrimaryChatModel(selectedModel, modelsData),
    [selectedModel, modelsData],
  );

  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [editingImages, setEditingImages] = useState<string[]>([]);
  const [uploadedImages, setUploadedImages] = useState<string[]>([]);
  const [uploadedAudioFiles, setUploadedAudioFiles] = useState<Array<{ data: string; mime: string; name: string }>>([]);
  const [expandedThinking, setExpandedThinking] = useState<Set<number>>(new Set());
  const [isUserAtBottom, setIsUserAtBottom] = useState(true);
  const [lemonadeTools, setLemonadeTools] = useState<LemonadeToolsResult | null>(null);
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

  useEffect(() => {
    const decodePrompt = (raw: string) => {
      try {
        return decodeURIComponent(raw.replace(/\+/g, ' '));
      } catch {
        return raw;
      }
    };

    const getPromptFromLocation = () => {
      const params = new URLSearchParams(window.location.search);
      const qPrompt = params.get('q');
      if (qPrompt && qPrompt.trim().length > 0) {
        return decodePrompt(qPrompt);
      }

      const entries = Array.from(params.entries()).filter(([, value]) => value?.trim().length > 0);
      if (entries.length === 1) {
        return decodePrompt(entries[0][1]);
      }

      return '';
    };

    const applyPromptFromQuery = () => {
      const fromHeader = (window as any).__LEMONADE_INITIAL_PROMPT__;
      const queryPrompt = getPromptFromLocation();
      const prompt = typeof fromHeader === 'string' && fromHeader.trim().length > 0
        ? decodePrompt(fromHeader)
        : queryPrompt;
      if (!prompt || prompt.trim().length === 0) return;

      setInputValue(prompt);
      delete (window as any).__LEMONADE_INITIAL_PROMPT__;

      window.requestAnimationFrame(() => {
        if (!inputTextareaRef.current) return;
        adjustTextareaHeight(inputTextareaRef.current);
        inputTextareaRef.current.focus();
        const len = inputTextareaRef.current.value.length;
        inputTextareaRef.current.setSelectionRange(len, len);
      });
    };

    applyPromptFromQuery();
    window.addEventListener('popstate', applyPromptFromQuery);

    return () => {
      window.removeEventListener('popstate', applyPromptFromQuery);
    };
  }, []);

  // Build lemonade tools when experience mode activates
  useEffect(() => {
    if (!experienceMode || !modelsData[selectedModel]) {
      setLemonadeTools(null);
      return;
    }
    setLemonadeTools(buildLemonadeTools(selectedModel, modelsData));
  }, [experienceMode, selectedModel, modelsData]);

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
    model: chatModelName,
    messages: messageHistory,
    stream: true,
    ...buildChatRequestOverrides(appSettings),
  });

  /** Build an error message enriched with backend action help text when available. */
  const buildErrorMessage = (error: any): string => {
    const errorMessage = error.message || 'Failed to get response from the model.';
    const modelInfo = modelsData[chatModelName];
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

  /**
   * Client-side agentic loop for experience mode.
   * Calls LLM with tools, executes tool_calls locally via Lemonade endpoints, loops.
   */
  const handleExperienceChat = async (messageHistory: Message[]): Promise<void> => {
    if (!lemonadeTools) throw new Error('Lemonade tools not loaded');
    const MAX_ITERATIONS = 5;
    const isNewModelLoad = currentLoadedModel !== chatModelName;

    // Pre-extract audio and image data from user messages
    const extractedAudio: Array<{ data: string; mime: string }> = [];
    const extractedImages: Array<{ dataUrl: string }> = [];

    const processedMessages: any[] = messageHistory.map(msg => {
      if (typeof msg.content === 'string') {
        return { role: msg.role, content: msg.content };
      }
      // Content is an array — strip binary data (images/audio) from all messages
      // For user messages: extract into context for tool use
      // For assistant messages: replace with placeholders so LLM doesn't choke
      const isUser = msg.role === 'user';
      const newContent: any[] = [];
      for (const item of msg.content) {
        if (item.type === 'audio' && 'audio' in item) {
          if (isUser) {
            extractedAudio.push({ data: item.audio.data, mime: item.audio.mime });
            newContent.push({ type: 'text', text: `[User provided audio file #${extractedAudio.length}]` });
          }
          // Drop audio from assistant messages
        } else if (item.type === 'image_url' && 'image_url' in item) {
          if (isUser) {
            extractedImages.push({ dataUrl: item.image_url.url });
            newContent.push({ type: 'text', text: `[User provided image #${extractedImages.length}]` });
          } else {
            newContent.push({ type: 'text', text: '[Generated image]' });
          }
        } else {
          newContent.push(item);
        }
      }
      // Simplify single-text arrays
      if (newContent.length === 1 && newContent[0].type === 'text') {
        return { role: msg.role, content: newContent[0].text };
      }
      return { role: msg.role, content: newContent.length > 0 ? newContent : '' };
    });

    // Prepend system prompt
    if (lemonadeTools.systemPrompt) {
      if (processedMessages.length > 0 && processedMessages[0].role === 'system') {
        processedMessages[0].content = lemonadeTools.systemPrompt + '\n\n' + processedMessages[0].content;
      } else {
        processedMessages.unshift({ role: 'system', content: lemonadeTools.systemPrompt });
      }
    }

    // Seed artifacts with only the most recent image from conversation history
    // (needed for edit_image auto-routing). Audio is not carried forward — each TTS is independent.
    const artifacts: Artifact[] = [];

    // Find the last image in prior messages (scan in reverse)
    let seededImage = false;
    for (let i = messageHistory.length - 1; i >= 0 && !seededImage; i--) {
      const msg = messageHistory[i];
      if (typeof msg.content === 'string') continue;
      for (let j = msg.content.length - 1; j >= 0 && !seededImage; j--) {
        const item = msg.content[j];
        if (item.type === 'image_url' && 'image_url' in item) {
          const url = item.image_url.url;
          const b64Marker = ';base64,';
          const b64Pos = url.indexOf(b64Marker);
          if (b64Pos !== -1) {
            artifacts.push({
              type: 'image',
              data: url.substring(b64Pos + b64Marker.length),
              mime: url.substring(5, b64Pos),
            });
            seededImage = true;
          }
        }
      }
    }

    // Also seed from user-uploaded images extracted this turn
    for (const img of extractedImages) {
      const b64Marker = ';base64,';
      const b64Pos = img.dataUrl.indexOf(b64Marker);
      if (b64Pos !== -1) {
        const b64Data = img.dataUrl.substring(b64Pos + b64Marker.length);
        let mime = 'image/png';
        if (img.dataUrl.length > 5) {
          mime = img.dataUrl.substring(5, b64Pos); // skip "data:"
        }
        artifacts.push({ type: 'image', data: b64Data, mime });
      }
    }

    // Agentic loop
    const llmMessages = [...processedMessages];

    for (let iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
      // Show thinking indicator while waiting for LLM response
      const thinkingText = iteration === 0 ? 'Thinking...' : 'Thinking...';
      setMessages(prev => {
        const newMessages = [...prev];
        newMessages[newMessages.length - 1] = {
          role: 'assistant',
          content: buildFinalContent(thinkingText, artifacts),
        };
        return newMessages;
      });

      const requestBody = {
        model: chatModelName,
        messages: llmMessages,
        stream: false,
        tools: lemonadeTools.tools,
        ...buildChatRequestOverrides(appSettings),
      };

      const response = await serverFetch('/chat/completions', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(requestBody),
        signal: abortControllerRef.current?.signal,
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      const data = await response.json();

      // Notify UI that model is loaded on first response
      if (iteration === 0) {
        setCurrentLoadedModel(chatModelName);
        if (isNewModelLoad) {
          window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: selectedModel } }));
        }
      }

      if (data.error) throw new Error(data.error.message || 'LLM returned error');
      if (!data.choices?.length) throw new Error('LLM returned empty response');

      const assistantMsg = data.choices[0].message;

      // No tool_calls → final response
      if (!assistantMsg.tool_calls || assistantMsg.tool_calls.length === 0) {
        const text = assistantMsg.content || '';
        const finalContent = buildFinalContent(text, artifacts);
        setMessages(prev => {
          const newMessages = [...prev];
          newMessages[newMessages.length - 1] = { role: 'assistant', content: finalContent };
          return newMessages;
        });
        return;
      }

      // Process tool calls
      llmMessages.push(assistantMsg);

      for (const toolCall of assistantMsg.tool_calls) {
        const funcName = toolCall.function.name;
        const toolModel = lemonadeTools.models[funcName];

        let resultContent: string;
        if (toolModel) {
          try {
            const context: ToolExecutionContext = {
              extractedAudio,
              extractedImages,
              previousArtifacts: artifacts,
            };
            const result = await executeLemonadeTool(toolCall, toolModel, context, modelsData);

            if (result.type === 'image' && result.data) {
              // For edits, replace the last image; for generation, append
              let lastImageIdx = -1;
              for (let i = artifacts.length - 1; i >= 0; i--) {
                if (artifacts[i].type === 'image') { lastImageIdx = i; break; }
              }
              if (funcName === 'edit_image' && lastImageIdx !== -1) {
                artifacts[lastImageIdx] = { type: 'image', data: result.data, mime: result.mime || 'image/png' };
              } else {
                artifacts.push({ type: 'image', data: result.data, mime: result.mime || 'image/png' });
              }
              resultContent = funcName === 'edit_image' ? 'Image edited successfully.' : 'Image generated successfully.';
            } else if (result.type === 'audio' && result.data) {
              artifacts.push({ type: 'audio', data: result.data, mime: result.mime || 'audio/wav' });
              resultContent = 'Audio generated successfully.';
            } else {
              resultContent = result.text || 'Tool executed successfully.';
            }
          } catch (err: any) {
            resultContent = `Error: ${err.message || 'Tool execution failed'}`;
          }
        } else {
          resultContent = `Error: Unknown tool '${funcName}'`;
        }

        llmMessages.push({
          role: 'tool',
          tool_call_id: toolCall.id,
          content: resultContent,
        });

        // Update UI with artifacts as they come in
        const text = assistantMsg.content || '';
        setMessages(prev => {
          const newMessages = [...prev];
          newMessages[newMessages.length - 1] = {
            role: 'assistant',
            content: buildFinalContent(text, artifacts),
          };
          return newMessages;
        });
      }
    }

    // Max iterations reached — show whatever we have
    const fallbackText = artifacts.length === 0 ? 'Sorry, I was unable to complete that request.' : '';
    setMessages(prev => {
      const newMessages = [...prev];
      newMessages[newMessages.length - 1] = {
        role: 'assistant',
        content: buildFinalContent(fallbackText, artifacts),
      };
      return newMessages;
    });
  };

  /** Build MessageContent from artifacts + text */
  const buildFinalContent = (text: string, artifacts: Artifact[]): MessageContent => {
    if (artifacts.length === 0) return text;
    const contentArray: Array<TextContent | ImageContent | AudioContent> = [];
    for (const artifact of artifacts) {
      if (artifact.type === 'image') {
        contentArray.push({
          type: 'image_url',
          image_url: { url: `data:${artifact.mime};base64,${artifact.data}` },
        });
      } else if (artifact.type === 'audio') {
        contentArray.push({
          type: 'audio',
          audio: { data: artifact.data, mime: artifact.mime },
        });
      }
    }
    if (text) contentArray.push({ type: 'text', text });
    return contentArray;
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
    const isNewModelLoad = currentLoadedModel !== chatModelName;

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

    const requestBody = buildChatRequestBody(messageHistory);
    console.log('[LLMChat] sending chat request:', {
      model: requestBody.model,
      messageCount: requestBody.messages.length,
      stream: requestBody.stream,
    });

    const response = await serverFetch('/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: abortControllerRef.current!.signal,
    });

    console.log('[LLMChat] response status:', response.status, response.statusText);
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    if (!response.body) throw new Error('Response body is null');

    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    // Buffer incomplete lines across chunks. webkit2gtk (Tauri on Linux)
    // delivers fetch ReadableStream chunks with different boundaries than
    // Chromium, so an SSE `data: {...}` payload may be split across two
    // reads. Without this buffer the second half lacks the `data: ` prefix
    // and gets silently discarded — manifesting as "only the first token"
    // or "No content received from stream".
    let lineBuffer = '';

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        lineBuffer += decoder.decode(value, { stream: true });
        const lines = lineBuffer.split('\n');
        // Keep the last (potentially incomplete) line in the buffer.
        lineBuffer = lines.pop() || '';

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
                  setCurrentLoadedModel(chatModelName);
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

  const sendMessage = async (textOverride?: string) => {
    const textToSend = typeof textOverride === 'string' ? textOverride : inputValue;
    // When called from voice auto-submit, `isBusy` may still be stale-true
    // because the state update hasn't flushed yet.
    if (!textToSend.trim() && uploadedImages.length === 0 && uploadedAudioFiles.length === 0) return;

    console.log('[LLMChat] sendMessage called', {
      chatModelName, selectedModel, experienceMode,
      isBusy, isPreFlight, isInferring,
    });

    const ready = await runPreFlight('llm', {
      modelName: chatModelName,
      modelsData,
      onError: (msg) => {
        console.error('[LLMChat] pre-flight error:', msg);
        setMessages(prev => [...prev, { role: 'assistant', content: `Error preparing model: ${msg}` }]);
      },
    });
    console.log('[LLMChat] pre-flight result:', ready);
    if (!ready) return;

    if (abortControllerRef.current) abortControllerRef.current.abort();
    abortControllerRef.current = new AbortController();
    setIsUserAtBottom(true);
    userScrolledAwayRef.current = false;

    let messageContent: MessageContent;
    if (uploadedImages.length > 0 || uploadedAudioFiles.length > 0) {
      const contentArray: Array<TextContent | ImageContent | AudioContent> = [];
      if (textToSend.trim()) contentArray.push({ type: 'text', text: textToSend });
      uploadedImages.forEach(imageUrl => {
        contentArray.push({ type: 'image_url', image_url: { url: imageUrl } });
      });
      uploadedAudioFiles.forEach(audio => {
        contentArray.push({ type: 'audio', audio: { data: audio.data, mime: audio.mime, name: audio.name } });
      });
      messageContent = contentArray;
    } else {
      messageContent = textToSend;
    }

    const userMessage: Message = { role: 'user', content: messageContent };
    const messageHistory = [...messages, userMessage];

    setMessages(prev => [...prev, userMessage]);
    setInputValue('');
    setUploadedImages([]);
    setUploadedAudioFiles([]);
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    try {
      if (experienceMode && lemonadeTools) {
        await handleExperienceChat(messageHistory);
      } else {
        await handleStreamingResponse(messageHistory);
      }
    } catch (error: any) {
      if (error.name === 'AbortError') {
        console.log('Request aborted - keeping partial response');
        setMessages(prev => {
          const lastMessage = prev[prev.length - 1];
          if (!lastMessage || (!lastMessage.content && !lastMessage.thinking)) return prev.slice(0, -1);
          return prev;
        });
      } else {
        console.error('[LLMChat] sendMessage error:', error?.name, error?.message, error);
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
      modelName: chatModelName,
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
      if (experienceMode && lemonadeTools) {
        await handleExperienceChat(messageHistory);
      } else {
        await handleStreamingResponse(messageHistory);
      }
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

  const renderMessageContent = (content: MessageContent, thinking?: string, messageIndex?: number, isComplete?: boolean, role?: string) => (
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
            if (item.type === 'image_url') {
              const url = item.image_url.url;
              if (!url.startsWith('data:image/')) return null;
              if (role === 'assistant') {
                return (
                  <div key={index} className="image-generation-item">
                    <div className="generated-images-row">
                      <div className="generated-image-column">
                        <div className="image-wrapper">
                          <img src={url} alt="Generated" className="generated-image" />
                        </div>
                      </div>
                    </div>
                  </div>
                );
              }
              return <img key={index} src={url} alt="Uploaded" className="message-image" />;
            }
            if (item.type === 'audio') {
              const audioItem = item as AudioContent;
              const fileName = audioItem.audio.name;
              if (fileName) {
                return (
                  <div key={index} className="message-audio-file">
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                      <path d="M9 18V5l12-2v13" /><circle cx="6" cy="18" r="3" /><circle cx="18" cy="16" r="3" />
                    </svg>
                    <span className="message-audio-filename">{fileName}</span>
                  </div>
                );
              }
              const SAFE_AUDIO_MIMES = ['audio/wav', 'audio/mpeg', 'audio/mp3', 'audio/ogg', 'audio/flac', 'audio/webm', 'audio/m4a', 'audio/mp4'];
              const safeMime = SAFE_AUDIO_MIMES.includes(audioItem.audio.mime) ? audioItem.audio.mime : 'audio/wav';
              return (
                <div key={index} className="message-audio">
                  <audio controls src={`data:${safeMime};base64,${audioItem.audio.data}`} />
                </div>
              );
            }
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
    <div className="llm-chat-panel">
      <div className="chat-header">
        <h3>LLM Chat</h3>
        <button
          className="new-chat-button"
          onClick={onNewChat}
          disabled={isBusy}
          title="Start a new chat"
        >
          <RefreshIcon />
        </button>
      </div>
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
                  {renderMessageContent(message.content, message.thinking, index, message.role === 'assistant', message.role)}
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
          {uploadedAudioFiles.length > 0 && (
            <div className="audio-preview-list">
              {uploadedAudioFiles.map((audio, index) => (
                <div key={index} className="audio-preview-item">
                  <svg className="audio-preview-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                    <path d="M9 18V5l12-2v13" /><circle cx="6" cy="18" r="3" /><circle cx="18" cy="16" r="3" />
                  </svg>
                  <span className="audio-preview-name">{audio.name}</span>
                  <button className="audio-preview-remove" onClick={() => setUploadedAudioFiles(prev => prev.filter((_, i) => i !== index))} title="Remove audio file">×</button>
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
            onPaste={uploadedImageHandlers.paste}
            placeholder="Type your message..."
            rows={1}
          />
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={activeModality === 'llm'}
            onSend={sendMessage}
            onStop={handleStopGeneration}
            sendDisabled={!inputValue.trim() && uploadedImages.length === 0 && uploadedAudioFiles.length === 0}
            modelSelector={<ModelSelector disabled={isBusy} />}
            leftControls={(
              <>
                {isVision && (
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
                )}
                <RecordButton
                  disabled={isBusy}
                  inputValue={inputValue}
                  setInputValue={setInputValue}
                  textareaRef={inputTextareaRef}
                  onError={showError}
                  runPreFlight={runPreFlight}
                  reset={reset}
                  onAutoSubmit={(text) => sendMessage(text)}
                />
              </>
            )}
          />
        </div>
      </div>
    </div>
  );
};

export default LLMChatPanel;
