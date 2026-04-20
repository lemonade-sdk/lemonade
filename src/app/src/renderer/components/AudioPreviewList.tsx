import React from 'react';
import { UploadedAudio } from '../utils/chatTypes';

interface AudioPreviewListProps {
  audio: UploadedAudio[];
  onRemove: (index: number) => void;
  className?: string;
}

const AudioPreviewList: React.FC<AudioPreviewListProps> = ({
  audio,
  onRemove,
  className = 'audio-preview-container',
}) => {
  if (audio.length === 0) return null;

  return (
    <div className={className}>
      {audio.map((item, index) => (
        <div key={index} className="audio-preview-item">
          <audio controls src={item.dataUrl} className="audio-preview" />
          <span className="audio-preview-name" title={item.filename}>
            {item.filename}
          </span>
          <button
            className="audio-remove-button"
            onClick={() => onRemove(index)}
            title="Remove audio"
          >
            ×
          </button>
        </div>
      ))}
    </div>
  );
};

export default AudioPreviewList;
