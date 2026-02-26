export const adjustTextareaHeight = (textarea: HTMLTextAreaElement) => {
  textarea.style.height = 'auto';
  const maxHeight = 200;
  const newHeight = Math.min(textarea.scrollHeight, maxHeight);
  textarea.style.height = newHeight + 'px';
  textarea.style.overflowY = textarea.scrollHeight > maxHeight ? 'auto' : 'hidden';
};
