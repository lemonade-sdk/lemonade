import React from 'react';
import { MicrophoneIcon } from './Icons';
import { Modality } from '../hooks/useInferenceState';
import { ModelsData } from '../utils/modelData';
import { useVoiceTranscription } from '../hooks/useVoiceTranscription';

interface RecordButtonProps {
  disabled?: boolean;
  inputValue: string;
  setInputValue: (value: string) => void;
  textareaRef?: React.RefObject<HTMLTextAreaElement | null>;
  onError: (error: string) => void;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  onAutoSubmit?: (text: string) => void;
}

const RecordButton: React.FC<RecordButtonProps> = (props) => {
  const { activeModel, isRecording, start, stop } =
    useVoiceTranscription(props);

  const title = !activeModel
    ? 'No Whisper model available'
    : isRecording ? 'Stop recording'
    : 'Record voice (auto-loads Whisper)';

  return (
    <button
      className={`audio-file-button${isRecording ? ' recording' : ''}`}
      onClick={isRecording ? stop : start}
      disabled={!isRecording && props.disabled}
      title={title}
    >
      <MicrophoneIcon active={isRecording} />
    </button>
  );
};

export default RecordButton;
