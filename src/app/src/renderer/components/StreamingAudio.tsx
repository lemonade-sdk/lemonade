import React, { useEffect, useMemo, useRef } from 'react';

interface StreamingAudioProps {
  /** base64-encoded audio payload */
  data: string;
  /** mime type, e.g. "audio/wav" */
  mime: string;
}

function base64ToBlobUrl(data: string, mime: string): string {
  const binary = atob(data);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return URL.createObjectURL(new Blob([bytes], { type: mime }));
}

/**
 * Renders generated TTS audio as an <audio> element backed by a blob URL
 * instead of a data URL. Data URLs in some webviews leave the progress bar
 * stuck at the end after playback, and don't allow the user to scrub back
 * reliably. Blob URLs behave like normal media. Also auto-plays on mount
 * so new audio starts as soon as the agent finishes generating it.
 */
const StreamingAudio: React.FC<StreamingAudioProps> = ({ data, mime }) => {
  const url = useMemo(() => base64ToBlobUrl(data, mime), [data, mime]);
  const ref = useRef<HTMLAudioElement | null>(null);

  useEffect(() => {
    return () => URL.revokeObjectURL(url);
  }, [url]);

  return (
    <audio
      ref={ref}
      controls
      autoPlay
      src={url}
    />
  );
};

export default StreamingAudio;
