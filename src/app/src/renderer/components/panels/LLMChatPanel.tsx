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
import { Message, MessageContent, TextContent, ImageContent, AudioContent, UploadedAudio, Artifact } from '../../utils/chatTypes';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import { SendIcon, ImageUploadIcon, AudioUploadIcon, RefreshIcon, EjectIcon } from '../Icons';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import ImagePreviewList from '../ImagePreviewList';
import AudioPreviewList from '../AudioPreviewList';
import EmptyState from '../EmptyState';
import ImageLightbox from '../ImageLightbox';
import StreamingAudio from '../StreamingAudio';
import TypingIndicator from '../TypingIndicator';
import { getCollectionPrimaryChatModel } from '../../utils/collectionModels';
import RecordButton from '../RecordButton';
import {
  buildLemonadeTools,
  executeLemonadeTool,
  LemonadeToolsResult,
  ToolExecutionContext,
} from '../../utils/lemonadeTools';
import { useAudioCapture } from '../../hooks/useAudioCapture';
import { encodeWAV, base64ToPlaybackUrl } from '../../utils/audioUtils';

// WebView2 won't play large `data:audio/...;base64,...` URLs reliably, so we
// feed `<audio>` a blob URL derived from the same base64 and revoke on unmount.
const MessageAudio: React.FC<{ data: string; format: string }> = ({ data, format }) => {
  const url = useMemo(() => base64ToPlaybackUrl(data, format), [data, format]);
  useEffect(() => () => URL.revokeObjectURL(url), [url]);
  return <audio controls src={url} className="message-audio" />;
};

// Map MIME type or filename extension to the OpenAI `input_audio.format` value.
const AUDIO_FORMAT_BY_MIME: Record<string, string> = {
  'audio/wav': 'wav',
  'audio/x-wav': 'wav',
  'audio/wave': 'wav',
  'audio/mpeg': 'mp3',
  'audio/mp3': 'mp3',
  'audio/mp4': 'm4a',
  'audio/x-m4a': 'm4a',
  'audio/flac': 'flac',
  'audio/x-flac': 'flac',
  'audio/ogg': 'ogg',
  'audio/webm': 'webm',
};

const AUDIO_FORMAT_BY_EXT: Record<string, string> = {
  wav: 'wav',
  mp3: 'mp3',
  m4a: 'm4a',
  mp4: 'm4a',
  flac: 'flac',
  ogg: 'ogg',
  oga: 'ogg',
  webm: 'webm',
};

function resolveAudioFormat(file: File): string {
  const byMime = AUDIO_FORMAT_BY_MIME[file.type.toLowerCase()];
  if (byMime) return byMime;
  const ext = file.name.toLowerCase().split('.').pop();
  if (ext && AUDIO_FORMAT_BY_EXT[ext]) return AUDIO_FORMAT_BY_EXT[ext];
  return 'wav';
}

// Inverse of AUDIO_FORMAT_BY_MIME — given an OpenAI input_audio.format like
// "mp3", return the MIME to feed <audio>/Blob. Single source of truth so
// handleCollectionChat, buildFinalContent, and renderMessageContent don't
// all carry their own redundant literal map.
const AUDIO_MIME_BY_FORMAT: Record<string, string> = {
  wav: 'audio/wav',
  mp3: 'audio/mpeg',
  m4a: 'audio/mp4',
  flac: 'audio/flac',
  ogg: 'audio/ogg',
  webm: 'audio/webm',
};

function mimeForFormat(fmt?: string): string {
  return (fmt && AUDIO_MIME_BY_FORMAT[fmt]) || 'audio/wav';
}

function formatForMime(mime?: string): string {
  return (mime && AUDIO_FORMAT_BY_MIME[mime.toLowerCase()]) || 'wav';
}

function splitDataUrl(dataUrl: string): { base64: string } {
  const comma = dataUrl.indexOf(',');
  return { base64: comma >= 0 ? dataUrl.slice(comma + 1) : dataUrl };
}

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
  isAudioChat?: boolean;
  collectionMode?: boolean;
  currentLoadedModel: string | null;
  setCurrentLoadedModel: React.Dispatch<React.SetStateAction<string | null>>;
  onNewChat?: () => void;
  onUnloadCollection?: () => void;
}

const LLMChatPanel: React.FC<LLMChatPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality,
  runPreFlight, reset, showError, appSettings,
  isVision, isAudioChat = false, collectionMode = false, currentLoadedModel, setCurrentLoadedModel,
  onNewChat, onUnloadCollection,
}) => {
  const { selectedModel, modelsData } = useModels();
  const { systemInfo } = useSystem();
  const tts = useTTS(appSettings, modelsData);
  const chatModelName = useMemo(
    () => getCollectionPrimaryChatModel(selectedModel, modelsData),
    [selectedModel, modelsData],
  );

  const [messages, setMessages] = useState<Message[]>([]);
  const [inputValue, setInputValue] = useState('');
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [editingImages, setEditingImages] = useState<string[]>([]);
  const [uploadedImages, setUploadedImages] = useState<string[]>([]);
  const [editingAudio, setEditingAudio] = useState<UploadedAudio[]>([]);
  const [uploadedAudio, setUploadedAudio] = useState<UploadedAudio[]>([]);
  const [showAudioMenu, setShowAudioMenu] = useState(false);
  const [showEditAudioMenu, setShowEditAudioMenu] = useState(false);
  const [isRecordingAudio, setIsRecordingAudio] = useState(false);
  const audioMenuRef = useRef<HTMLDivElement>(null);
  const editAudioMenuRef = useRef<HTMLDivElement>(null);
  const [expandedThinking, setExpandedThinking] = useState<Set<number>>(new Set());
  const [isUserAtBottom, setIsUserAtBottom] = useState(true);
  const [lemonadeTools, setLemonadeTools] = useState<LemonadeToolsResult | null>(null);
  const [lightboxSrc, setLightboxSrc] = useState<string | null>(null);
  const userScrolledAwayRef = useRef(false);

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const editFileInputRef = useRef<HTMLInputElement>(null);
  const audioInputRef = useRef<HTMLInputElement>(null);
  const editAudioInputRef = useRef<HTMLInputElement>(null);
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

  // Build lemonade tools when collection mode activates
  useEffect(() => {
    if (!collectionMode || !modelsData[selectedModel]) {
      setLemonadeTools(null);
      return;
    }
    setLemonadeTools(buildLemonadeTools(selectedModel, modelsData));
  }, [collectionMode, selectedModel, modelsData]);

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

  // Audio handlers — mirror the image handlers. Preview plays from a blob URL
  // (WebView2 won't reliably play long `data:audio/...` URLs); the bare base64
  // goes to the API in `input_audio.data`.
  const createAudioHandlers = (setAudio: React.Dispatch<React.SetStateAction<UploadedAudio[]>>) => ({
    upload: (event: React.ChangeEvent<HTMLInputElement>) => {
      const files = event.target.files;
      if (!files || files.length === 0) return;
      const file = files[0];
      const format = resolveAudioFormat(file);
      const playbackUrl = URL.createObjectURL(file);
      const reader = new FileReader();
      reader.onload = (e) => {
        const result = e.target?.result;
        if (typeof result !== 'string') return;
        const { base64 } = splitDataUrl(result);
        setAudio(prev => [...prev, { dataUrl: playbackUrl, base64, format, filename: file.name }]);
      };
      reader.readAsDataURL(file);
      event.target.value = '';
    },
    remove: (index: number) => {
      setAudio(prev => prev.filter((_, i) => i !== index));
    },
  });

  const uploadedAudioHandlers = createAudioHandlers(setUploadedAudio);
  const editingAudioHandlers = createAudioHandlers(setEditingAudio);

  // Mic-record-to-input_audio: accumulate PCM chunks from useAudioCapture,
  // then pack them into a WAV on stop and add as an UploadedAudio entry.
  const audioChunksRef = useRef<string[]>([]);
  const recordTargetRef = useRef<React.Dispatch<React.SetStateAction<UploadedAudio[]>>>(setUploadedAudio);
  const audioCapture = useAudioCapture(
    (base64Chunk) => { audioChunksRef.current.push(base64Chunk); },
  );

  const startAudioRecording = (target: React.Dispatch<React.SetStateAction<UploadedAudio[]>>) => {
    audioChunksRef.current = [];
    recordTargetRef.current = target;
    setIsRecordingAudio(true);
    audioCapture.startRecording();
  };

  const stopAudioRecording = () => {
    audioCapture.stopRecording();
    setIsRecordingAudio(false);
    const chunks = audioChunksRef.current;
    if (chunks.length === 0) return;
    const { wavBase64, playbackUrl, durationSeconds } = encodeWAV(chunks);
    const label = `recording-${Math.round(durationSeconds)}s.wav`;
    recordTargetRef.current(prev => [...prev, {
      dataUrl: playbackUrl,
      base64: wavBase64,
      format: 'wav',
      filename: label,
    }]);
    audioChunksRef.current = [];
  };

  // Close audio menus on outside click
  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      if (audioMenuRef.current && !audioMenuRef.current.contains(e.target as Node)) {
        setShowAudioMenu(false);
      }
      if (editAudioMenuRef.current && !editAudioMenuRef.current.contains(e.target as Node)) {
        setShowEditAudioMenu(false);
      }
    };
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

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
   * Client-side agentic loop for collection mode.
   * Calls LLM with tools, executes tool_calls locally via Lemonade endpoints, loops.
   */
  const handleCollectionChat = async (messageHistory: Message[]): Promise<void> => {
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
        if (item.type === 'input_audio' && 'input_audio' in item) {
          if (isUser) {
            const mime = mimeForFormat(item.input_audio.format);
            extractedAudio.push({ data: item.input_audio.data, mime });
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

    // sourceArtifacts: images from prior history / current user upload — used as
    // tool input context (e.g. edit_image source). Not rendered in the assistant
    // response; the user's message already shows their upload.
    const sourceArtifacts: Artifact[] = [];

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
            sourceArtifacts.push({
              type: 'image',
              data: url.substring(b64Pos + b64Marker.length),
              mime: url.substring(5, b64Pos),
            });
            seededImage = true;
          }
        }
      }
    }

    // artifacts: generated this turn — rendered in the assistant response.
    const artifacts: Artifact[] = [];

    // Agentic loop
    const llmMessages = [...processedMessages];

    for (let iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
      // Show thinking indicator while waiting for LLM response
      const thinkingText = 'Thinking...';
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
              previousArtifacts: [...sourceArtifacts, ...artifacts],
            };
            const result = await executeLemonadeTool(toolCall, toolModel, context, modelsData, abortControllerRef.current?.signal);

            if (result.type === 'image' && result.data) {
              // For edits, replace the last generated image from this turn if
              // one exists; otherwise append. The source image is never
              // rendered, so there's nothing to replace on the first edit.
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
        // Map the artifact's MIME to an input_audio `format`. The renderer
        // (MessageAudio / StreamingAudio) maps back to MIME for playback.
        contentArray.push({
          type: 'input_audio',
          input_audio: { data: artifact.data, format: formatForMime(artifact.mime) },
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

    const response = await serverFetch('/chat/completions', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: abortControllerRef.current!.signal,
    });

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
    if (!textToSend.trim() && uploadedImages.length === 0 && uploadedAudio.length === 0) return;

    const ready = await runPreFlight('llm', {
      modelName: chatModelName,
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
    if (uploadedImages.length > 0 || uploadedAudio.length > 0) {
      const contentArray: Array<TextContent | ImageContent | AudioContent> = [];
      if (textToSend.trim()) contentArray.push({ type: 'text', text: textToSend });
      uploadedImages.forEach(imageUrl => {
        contentArray.push({ type: 'image_url', image_url: { url: imageUrl } });
      });
      uploadedAudio.forEach(audio => {
        contentArray.push({
          type: 'input_audio',
          input_audio: { data: audio.base64, format: audio.format },
        });
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
    setUploadedAudio([]);
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    try {
      if (collectionMode && lemonadeTools) {
        await handleCollectionChat(messageHistory);
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
    if ((!editingValue.trim() && editingImages.length === 0 && editingAudio.length === 0) || editingIndex === null || isBusy) return;

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
    if (editingImages.length > 0 || editingAudio.length > 0) {
      const contentArray: Array<TextContent | ImageContent | AudioContent> = [];
      if (editingValue.trim()) contentArray.push({ type: 'text', text: editingValue });
      editingImages.forEach(imageUrl => {
        contentArray.push({ type: 'image_url', image_url: { url: imageUrl } });
      });
      editingAudio.forEach(audio => {
        contentArray.push({
          type: 'input_audio',
          input_audio: { data: audio.base64, format: audio.format },
        });
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
    setEditingAudio([]);
    setMessages(prev => [...prev, { role: 'assistant', content: '', thinking: '' }]);

    try {
      if (collectionMode && lemonadeTools) {
        await handleCollectionChat(messageHistory);
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

  const isImageOnlyAssistantMessage = (message: Message) => (
    collectionMode &&
    message.role === 'assistant' &&
    Array.isArray(message.content) &&
    message.content.length > 0 &&
    message.content.every((item) => item.type === 'image_url')
  );

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
              // Only accept base64-encoded image data URLs. Anything else
              // (javascript:, http:, etc.) is a CSS/XSS/exfil vector —
              // CodeQL flags it, and there's no legitimate chat path that
              // needs an arbitrary remote URL here.
              if (!url.startsWith('data:image/')) return null;
              if (role === 'assistant') {
                return (
                  <div key={index} className="collection-chat-image">
                    <img
                      src={url}
                      alt="Generated"
                      className="generated-image in-chat"
                      onClick={() => setLightboxSrc(url)}
                      title="Click to expand"
                    />
                  </div>
                );
              }
              return <img key={index} src={url} alt="Uploaded" className="message-image" />;
            }
            if (item.type === 'input_audio') {
              const fmt = item.input_audio.format || 'wav';
              // Assistant-generated TTS audio (from the collection agentic loop)
              // uses StreamingAudio for autoplay + scrubbing, while user input
              // audio uses the simpler MessageAudio playback.
              if (role === 'assistant') {
                return (
                  <div key={index} className="message-audio">
                    <StreamingAudio data={item.input_audio.data} mime={mimeForFormat(fmt)} />
                  </div>
                );
              }
              return <MessageAudio key={index} data={item.input_audio.data} format={fmt} />;
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
        setEditingAudio([]);
      } else {
        const textContent = message.content.find((item): item is TextContent => item.type === 'text');
        setEditingValue(textContent ? textContent.text : '');
        const imageContents = message.content.filter((item): item is ImageContent => item.type === 'image_url');
        setEditingImages(imageContents.map(img => img.image_url.url));
        const audioContents = message.content.filter((item): item is AudioContent => item.type === 'input_audio');
        setEditingAudio(audioContents.map((audio, i) => {
          const fmt = audio.input_audio.format || 'wav';
          return {
            dataUrl: base64ToPlaybackUrl(audio.input_audio.data, fmt),
            base64: audio.input_audio.data,
            format: fmt,
            filename: `audio-${i + 1}.${fmt}`,
          };
        }));
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
    setEditingAudio([]);
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
        <div className="chat-header-actions">
          {collectionMode && onUnloadCollection && (
            <button
              className="model-action-btn unload-btn active-model-eject-button collection-unload-icon-button"
              onClick={onUnloadCollection}
              disabled={isBusy}
              title="Eject collection"
              aria-label="Unload collection"
            >
              <EjectIcon />
            </button>
          )}
          <button
            className="new-chat-button"
            onClick={onNewChat}
            disabled={isBusy}
            title="Start a new chat"
          >
            <RefreshIcon />
          </button>
        </div>
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
          const isBubblelessImageMessage = isImageOnlyAssistantMessage(message);
          return (
            <div
              key={index}
              className={`chat-message ${message.role === 'user' ? 'user-message' : 'assistant-message'} ${
                message.role === 'user' && !isBusy ? 'editable' : ''
              } ${isGrayedOut ? 'grayed-out' : ''} ${editingIndex === index ? 'editing' : ''} ${
                isBubblelessImageMessage ? 'collection-image-message' : ''
              }`}
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
                  <AudioPreviewList
                    audio={editingAudio}
                    onRemove={editingAudioHandlers.remove}
                    className="edit-audio-preview-container"
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
                      {isAudioChat && (
                        <div className="audio-menu-wrapper" ref={editAudioMenuRef}>
                          <input
                            ref={editAudioInputRef}
                            type="file"
                            accept="audio/*"
                            onChange={(e) => { editingAudioHandlers.upload(e); setShowEditAudioMenu(false); }}
                            style={{ display: 'none' }}
                          />
                          {isRecordingAudio ? (
                            <button
                              className="audio-upload-button recording"
                              onClick={() => { stopAudioRecording(); setShowEditAudioMenu(false); }}
                              title="Stop recording"
                            >
                              <span className="recording-dot" />
                            </button>
                          ) : (
                            <button
                              className="audio-upload-button"
                              onClick={() => setShowEditAudioMenu(prev => !prev)}
                              title="Attach audio"
                            >
                              <AudioUploadIcon />
                            </button>
                          )}
                          {showEditAudioMenu && !isRecordingAudio && (
                            <div className="audio-dropdown-menu">
                              <button onClick={() => { editAudioInputRef.current?.click(); setShowEditAudioMenu(false); }}>
                                Upload file
                              </button>
                              <button onClick={() => { startAudioRecording(setEditingAudio); setShowEditAudioMenu(false); }}>
                                Record audio
                              </button>
                            </div>
                          )}
                        </div>
                      )}
                      <button
                        className="edit-send-button"
                        onClick={submitEdit}
                        disabled={!editingValue.trim() && editingImages.length === 0 && editingAudio.length === 0}
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
          <AudioPreviewList
            audio={uploadedAudio}
            onRemove={uploadedAudioHandlers.remove}
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
            stoppable={activeModality === 'llm'}
            onSend={sendMessage}
            onStop={handleStopGeneration}
            sendDisabled={!inputValue.trim() && uploadedImages.length === 0 && uploadedAudio.length === 0}
            modelSelector={<ModelSelector disabled={isBusy} />}
            rightControls={
              <RecordButton
                disabled={isBusy}
                inputValue={inputValue}
                setInputValue={setInputValue}
                textareaRef={inputTextareaRef}
                onError={showError}
                runPreFlight={runPreFlight}
                reset={reset}
              />
            }
            leftControls={
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
                {(isAudioChat || collectionMode) && (
                  <div className="audio-menu-wrapper" ref={audioMenuRef}>
                    <input
                      ref={audioInputRef}
                      type="file"
                      accept="audio/*"
                      onChange={(e) => { uploadedAudioHandlers.upload(e); setShowAudioMenu(false); }}
                      style={{ display: 'none' }}
                    />
                    {isRecordingAudio ? (
                      <button
                        className="audio-upload-button recording"
                        onClick={() => { stopAudioRecording(); setShowAudioMenu(false); }}
                        title="Stop recording"
                      >
                        <span className="recording-dot" />
                      </button>
                    ) : (
                      <button
                        className="audio-upload-button"
                        onClick={() => setShowAudioMenu(prev => !prev)}
                        disabled={isBusy}
                        title="Attach audio"
                      >
                        <AudioUploadIcon />
                      </button>
                    )}
                    {showAudioMenu && !isRecordingAudio && (
                      <div className="audio-dropdown-menu">
                        <button onClick={() => { audioInputRef.current?.click(); setShowAudioMenu(false); }}>
                          Upload file
                        </button>
                        <button onClick={() => { startAudioRecording(setUploadedAudio); setShowAudioMenu(false); }}>
                          Record audio
                        </button>
                      </div>
                    )}
                  </div>
                )}
              </>
            }
          />
        </div>
      </div>
      <ImageLightbox src={lightboxSrc} onClose={() => setLightboxSrc(null)} />
    </div>
  );
};

export default LLMChatPanel;
