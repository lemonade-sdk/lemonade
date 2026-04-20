/**
 * Pack accumulated base64-encoded PCM16 chunks into a single WAV file
 * (RIFF header + raw PCM data) and return the result as a base64 string.
 *
 * The chunks come from useAudioCapture which outputs 16-bit mono PCM at 16 kHz.
 *
 * Returns both a base64 payload (for the OpenAI `input_audio.data` field) and
 * an object URL suitable for use as an `<audio src>` — blob URLs avoid the
 * WebView2 oddities that cause multi-megabyte `data:` URLs to load silently
 * without becoming playable.
 */
export function encodeWAV(
  base64Chunks: string[],
  sampleRate = 16000,
  numChannels = 1,
  bitsPerSample = 16,
): { wavBase64: string; playbackUrl: string; durationSeconds: number } {
  const byteArrays = base64Chunks.map(b64ToBytesArray);
  const totalBytes = byteArrays.reduce((sum, a) => sum + a.length, 0);

  const header = new ArrayBuffer(44);
  const view = new DataView(header);

  const byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  const blockAlign = numChannels * (bitsPerSample / 8);

  writeString(view, 0, 'RIFF');
  view.setUint32(4, 36 + totalBytes, true);
  writeString(view, 8, 'WAVE');
  writeString(view, 12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, numChannels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, bitsPerSample, true);
  writeString(view, 36, 'data');
  view.setUint32(40, totalBytes, true);

  const wav = new Uint8Array(44 + totalBytes);
  wav.set(new Uint8Array(header), 0);
  let offset = 44;
  for (const arr of byteArrays) {
    wav.set(arr, offset);
    offset += arr.length;
  }

  const wavBase64 = uint8ToBase64(wav);
  const playbackUrl = URL.createObjectURL(new Blob([wav], { type: 'audio/wav' }));
  const durationSeconds = totalBytes / byteRate;
  return { wavBase64, playbackUrl, durationSeconds };
}

function writeString(view: DataView, offset: number, str: string) {
  for (let i = 0; i < str.length; i++) {
    view.setUint8(offset + i, str.charCodeAt(i));
  }
}

function b64ToBytesArray(base64: string): Uint8Array<ArrayBuffer> {
  const bin = atob(base64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

/**
 * Turn an OpenAI `input_audio.data` base64 payload into a blob URL suitable
 * for `<audio src>`. Same WebView2 reason as encodeWAV: big `data:` URLs
 * load silently without becoming playable.
 */
export function base64ToPlaybackUrl(base64: string, format: string): string {
  const bytes = b64ToBytesArray(base64);
  return URL.createObjectURL(new Blob([bytes], { type: `audio/${format}` }));
}

function uint8ToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}
