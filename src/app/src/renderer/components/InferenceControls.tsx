import React from 'react';
import { SendIcon, StopIcon } from './Icons';

interface InferenceControlsProps {
  isBusy: boolean;
  isInferring: boolean;
  /** Whether this modality supports stopping mid-inference */
  stoppable: boolean;
  onSend: () => void;
  onStop?: () => void;
  sendDisabled: boolean;
  sendIcon?: React.ReactNode;
  sendTitle?: string;
  leftControls?: React.ReactNode;
  modelSelector: React.ReactNode;
}

const TypingIndicatorSmall: React.FC = () => (
  <div className="typing-indicator small">
    <span></span>
    <span></span>
    <span></span>
  </div>
);

const InferenceControls: React.FC<InferenceControlsProps> = ({
  isBusy,
  isInferring,
  stoppable,
  onSend,
  onStop,
  sendDisabled,
  sendIcon,
  sendTitle,
  leftControls,
  modelSelector,
}) => {
  const renderActionButton = () => {
    if (isBusy && stoppable && isInferring && onStop) {
      return (
        <button className="chat-stop-button" onClick={onStop} title="Stop">
          <StopIcon />
        </button>
      );
    }

    if (isBusy) {
      return (
        <button className="chat-send-button" disabled title="Processing...">
          <TypingIndicatorSmall />
        </button>
      );
    }

    return (
      <button
        className="chat-send-button"
        onClick={onSend}
        disabled={sendDisabled}
        title={sendTitle || "Send"}
      >
        {sendIcon || <SendIcon />}
      </button>
    );
  };

  return (
    <div className="chat-controls">
      <div className="chat-controls-left">
        {leftControls}
        {modelSelector}
      </div>
      {renderActionButton()}
    </div>
  );
};

export default InferenceControls;
