import { useState, useRef, useCallback } from 'react';
import { serverFetch } from '../utils/serverConfig';

interface OmniToolResult {
  tool_call_id: string;
  tool_name: string;
  data: any;
  summary: string;
  success: boolean;
}

interface OmniStep {
  step_number: number;
  tool_calls: any[];
  results: OmniToolResult[];
}

export interface OmniMessage {
  role: 'user' | 'assistant';
  content: string;
  omniSteps?: OmniStep[];
  images?: string[];
  audioData?: { base64: string; format: string }[];
  attachedImages?: string[];
}

interface UseOmniChatReturn {
  messages: OmniMessage[];
  sendMessage: (text: string, model: string, images?: string[]) => Promise<void>;
  isProcessing: boolean;
  currentStep: string | null;
  clearMessages: () => void;
  abort: () => void;
}

export function useOmniChat(): UseOmniChatReturn {
  const [messages, setMessages] = useState<OmniMessage[]>([]);
  const [isProcessing, setIsProcessing] = useState(false);
  const [currentStep, setCurrentStep] = useState<string | null>(null);
  const abortControllerRef = useRef<AbortController | null>(null);

  const sendMessage = useCallback(async (text: string, model: string, images?: string[]) => {
    const userMessage: OmniMessage = {
      role: 'user',
      content: text,
      attachedImages: images?.length ? images : undefined,
    };
    setMessages(prev => [...prev, userMessage]);
    setIsProcessing(true);
    setCurrentStep(null);

    const allMessages = [...messages, userMessage];

    // Rebuild full conversation including tool call history so the LLM
    // has context about previous tool calls and their results.
    const apiMessages: any[] = [];
    for (const m of allMessages) {
      if (m.role === 'user') {
        // Build multimodal content array if user attached images
        if (m.attachedImages?.length) {
          apiMessages.push({
            role: 'user',
            content: [
              { type: 'text', text: m.content },
              ...m.attachedImages.map(url => ({ type: 'image_url', image_url: { url } })),
            ],
          });
        } else {
          apiMessages.push({ role: 'user', content: m.content });
        }
      } else if (m.role === 'assistant') {
        if (m.omniSteps && m.omniSteps.length > 0) {
          // Replay each step: assistant message with tool_calls, then tool results
          for (const step of m.omniSteps) {
            apiMessages.push({
              role: 'assistant',
              content: null,
              tool_calls: step.tool_calls,
            });
            for (const r of step.results) {
              apiMessages.push({
                role: 'tool',
                tool_call_id: r.tool_call_id,
                content: r.summary,
              });
            }
          }
          // Then the final assistant text response
          if (m.content) {
            apiMessages.push({ role: 'assistant', content: m.content });
          }
        } else {
          apiMessages.push({ role: 'assistant', content: m.content });
        }
      }
    }

    try {
      abortControllerRef.current = new AbortController();

      const response = await serverFetch('/omni/chat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model,
          messages: apiMessages,
          stream: true,
          omni: {
            tools: ['generate_image', 'describe_image', 'transcribe_audio', 'text_to_speech', 'edit_image',
                    'read_file', 'write_file', 'list_directory', 'web_search',
                    'list_models', 'load_model', 'run_command'],
          },
        }),
        signal: abortControllerRef.current.signal,
      });

      if (!response.ok) {
        const errorData = await response.json();
        throw new Error(errorData?.error?.message || `Server error: ${response.status}`);
      }

      const reader = response.body?.getReader();
      if (!reader) throw new Error('No response body');

      const decoder = new TextDecoder();
      let buffer = '';
      let assistantContent = '';
      let collectedSteps: OmniStep[] = [];
      let collectedImages: string[] = [];
      let collectedAudio: { base64: string; format: string }[] = [];
      let currentStepData: Partial<OmniStep> | null = null;
      // eventType must persist across reader chunks — large SSE payloads
      // (e.g. base64 images) can split event: and data: across reads.
      let eventType = '';

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';

        for (const line of lines) {
          if (line.startsWith('event: ')) {
            eventType = line.slice(7).trim();
          } else if (line.startsWith('data: ')) {
            const dataStr = line.slice(6);
            if (dataStr === '[DONE]') continue;

            try {
              const data = JSON.parse(dataStr);

              switch (eventType) {
                case 'omni.step.start':
                  currentStepData = {
                    step_number: data.step,
                    tool_calls: data.tool_calls,
                    results: [],
                  };
                  const toolNames = data.tool_calls?.map((tc: any) => tc.function?.name).join(', ') || 'tools';
                  setCurrentStep(`Using ${toolNames}...`);
                  break;

                case 'omni.step.result': {
                  const toolResult: OmniToolResult = {
                    tool_call_id: data.tool_call_id,
                    tool_name: data.tool_name,
                    data: data.data,
                    summary: data.summary,
                    success: data.success,
                  };

                  if (currentStepData) {
                    currentStepData.results = [...(currentStepData.results || []), toolResult];
                  }

                  if ((data.tool_name === 'generate_image' || data.tool_name === 'edit_image') && data.data?.data) {
                    for (const img of data.data.data) {
                      if (img.b64_json) {
                        collectedImages.push(img.b64_json);
                      }
                    }
                  }

                  if (data.tool_name === 'text_to_speech' && data.data?.audio_base64) {
                    collectedAudio.push({
                      base64: data.data.audio_base64,
                      format: data.data.format || 'mp3',
                    });
                  }

                  setCurrentStep(`${data.tool_name} completed`);
                  break;
                }

                case 'omni.step.complete':
                  if (currentStepData) {
                    collectedSteps.push(currentStepData as OmniStep);
                    currentStepData = null;
                  }
                  setCurrentStep(null);
                  break;

                case 'omni.response.delta':
                  assistantContent += data.content || '';
                  setMessages(prev => {
                    const lastMsg = prev[prev.length - 1];
                    if (lastMsg?.role === 'assistant') {
                      return [...prev.slice(0, -1), {
                        ...lastMsg,
                        content: assistantContent,
                      }];
                    }
                    return [...prev, {
                      role: 'assistant' as const,
                      content: assistantContent,
                      omniSteps: collectedSteps,
                      images: collectedImages,
                      audioData: collectedAudio,
                    }];
                  });
                  break;

                case 'omni.response.done':
                  if (data.omni_steps) {
                    collectedSteps = data.omni_steps;

                    // Re-extract images/audio from omni_steps in case
                    // step.result events were missed (e.g. chunked SSE).
                    for (const step of data.omni_steps) {
                      for (const r of step.results || []) {
                        if ((r.tool_name === 'generate_image' || r.tool_name === 'edit_image') && r.data?.data) {
                          for (const img of r.data.data) {
                            const b64 = img.b64_json;
                            if (b64 && !collectedImages.includes(b64)) {
                              collectedImages.push(b64);
                            }
                          }
                        }
                        if (r.tool_name === 'text_to_speech' && r.data?.audio_base64) {
                          const exists = collectedAudio.some(a => a.base64 === r.data.audio_base64);
                          if (!exists) {
                            collectedAudio.push({
                              base64: r.data.audio_base64,
                              format: r.data.format || 'mp3',
                            });
                          }
                        }
                      }
                    }
                  }

                  if (!assistantContent && data.choices?.[0]?.message?.content) {
                    assistantContent = data.choices[0].message.content;
                  }

                  setMessages(prev => {
                    const withoutLastAssistant = prev[prev.length - 1]?.role === 'assistant'
                      ? prev.slice(0, -1)
                      : prev;
                    return [...withoutLastAssistant, {
                      role: 'assistant' as const,
                      content: assistantContent,
                      omniSteps: collectedSteps,
                      images: collectedImages,
                      audioData: collectedAudio,
                    }];
                  });
                  break;
              }
            } catch {
              // Skip unparseable data lines
            }
          }
        }
      }

      if (assistantContent || collectedSteps.length > 0) {
        setMessages(prev => {
          const withoutLastAssistant = prev[prev.length - 1]?.role === 'assistant'
            ? prev.slice(0, -1)
            : prev;
          return [...withoutLastAssistant, {
            role: 'assistant' as const,
            content: assistantContent || '(Agent completed with no text response)',
            omniSteps: collectedSteps,
            images: collectedImages,
            audioData: collectedAudio,
          }];
        });
      }

    } catch (error: any) {
      if (error.name === 'AbortError') return;

      setMessages(prev => [...prev, {
        role: 'assistant' as const,
        content: `Error: ${error.message}`,
      }]);
    } finally {
      setIsProcessing(false);
      setCurrentStep(null);
      abortControllerRef.current = null;
    }
  }, [messages]);

  const clearMessages = useCallback(() => {
    setMessages([]);
    setCurrentStep(null);
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }
  }, []);

  const abort = useCallback(() => {
    abortControllerRef.current?.abort();
  }, []);

  return { messages, sendMessage, isProcessing, currentStep, clearMessages, abort };
}
