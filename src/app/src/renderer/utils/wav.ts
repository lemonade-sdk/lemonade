export const readWavFileAsBase64 = (file: File): Promise<string> =>
  new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error('Could not read the selected file.'));
    reader.onload = (ev) => {
      const buf = ev.target?.result as ArrayBuffer;
      const bytes = new Uint8Array(buf);
      const magic = (offset: number, text: string) =>
        bytes.length >= offset + text.length &&
        text.split('').every((c, i) => bytes[offset + i] === c.charCodeAt(0));
      if (!magic(0, 'RIFF') || !magic(8, 'WAVE')) {
        reject(new Error(`'${file.name}' is not a WAV file. Voice samples must be WAV audio.`));
        return;
      }
      let binary = '';
      const chunk = 0x8000;
      for (let i = 0; i < bytes.length; i += chunk) {
        binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
      }
      resolve(btoa(binary));
    };
    reader.readAsArrayBuffer(file);
  });

export const WAV_FILE_ACCEPT = '.wav,audio/wav,audio/x-wav';
