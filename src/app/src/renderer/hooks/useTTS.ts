import React, { useState, useEffect } from 'react';
import { LOADING, PAUSED, PLAYING } from '../AudioButton';
import { AppSettings } from '../utils/appSettings';
import { serverFetch } from '../utils/serverConfig';
import { ensureModelReady, DownloadAbortError } from '../utils/backendInstaller';
import { ModelsData } from '../utils/modelData';
import { MessageContent } from '../utils/chatTypes';

export function useTTS(appSettings: AppSettings | null, modelsData: ModelsData) {
  const [currentVoice, setVoice] = useState('');
  const [audioState, setAudioState] = useState<number>(0);
  const [pressedAudioButton, setPressedAudioButton] = useState<number>(-1);
  const [currentAudio, setCurrentAudio] = useState<HTMLAudioElement | null>(null);

  // Sync voice with settings
  useEffect(() => {
    if (appSettings) {
      setVoice(appSettings?.tts.userVoice.value);
    }
  }, [appSettings]);

  // Audio event listeners
  useEffect(() => {
    if (currentAudio) {
      currentAudio.addEventListener('ended', stopAudio);
      currentAudio.addEventListener('error', stopAudio);
      currentAudio.play().catch(e => console.error("Playback prevented:", currentAudio));
    }
    return () => {
      currentAudio?.removeEventListener('ended', stopAudio);
      currentAudio?.removeEventListener('error', stopAudio);
    };
  }, [currentAudio, audioState]);

  const playAudio = (audioUrl: string) => {
    setCurrentAudio(new Audio(audioUrl));
    setAudioState(PLAYING);
  };

  const stopAudio = () => {
    if (currentAudio) {
      currentAudio.pause();
      if (currentAudio.src.startsWith('blob:')) {
        URL.revokeObjectURL(currentAudio.src);
      }
      setCurrentAudio(null);
    }
    setPressedAudioButton(-1);
    setAudioState(PAUSED);
  };

  // Core TTS API call (no pre-flight)
  const doTextToSpeech = async (message: MessageContent, ttsVoice: string) => {
    setAudioState(LOADING);

    let textMessage: any = message;
    if (textMessage instanceof Array) {
      textMessage = textMessage.map(function(item: any) { return (item.type == "text") ? item.text : '' }).toString();
    }

    const textToSpeechModel = appSettings?.tts.model.value;
    const requestBody: any = {
      model: textToSpeechModel,
      input: textMessage,
      voice: ttsVoice
    };

    const response = await serverFetch('/audio/speech', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody)
    });

    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }

    const respBlob = await response.blob();
    const audioUrl = URL.createObjectURL(respBlob);
    playAudio(audioUrl);
    setAudioState(PLAYING);
  };

  // TTS with its own pre-flight (for inline audio buttons, independent of state machine)
  const handleTextToSpeech = async (message: MessageContent, ttsVoice: string) => {
    const textToSpeechModel = appSettings?.tts.model.value;

    if (textToSpeechModel) {
      try {
        await ensureModelReady(textToSpeechModel, modelsData);
      } catch (error: any) {
        if (error instanceof DownloadAbortError) return;
        console.error('TTS pre-flight failed:', error);
        stopAudio();
        return;
      }
    }

    try {
      await doTextToSpeech(message, ttsVoice);
    } catch (err: any) {
      console.log(`Error: ${err}`);
      stopAudio();
    }
  };

  const handleAudioButtonClick = async (message: MessageContent, btnIndex: number, role?: string) => {
    let b = pressedAudioButton;
    let voice = currentVoice;

    stopAudio();

    if (b === btnIndex) {
      return;
    }

    if (role && appSettings) {
      voice = (role === 'assistant') ? appSettings?.tts.assistantVoice.value : appSettings?.tts.userVoice.value;
    }

    setPressedAudioButton(btnIndex);
    await handleTextToSpeech(message, voice);
  };

  return {
    currentVoice, setVoice,
    audioState, pressedAudioButton, currentAudio,
    playAudio, stopAudio, doTextToSpeech,
    handleTextToSpeech, handleAudioButtonClick,
  };
}
