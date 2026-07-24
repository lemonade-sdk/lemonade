type SanitizeConfig = {
  ALLOWED_TAGS?: string[];
  ALLOWED_ATTR?: string[];
  ALLOW_DATA_ATTR?: boolean;
};

function isSafeUrl(value: string): boolean {
  const trimmed = value.trim().replace(/[\u0000-\u001f\u007f\s]+/g, '');
  if (!trimmed) return true;

  const lower = trimmed.toLowerCase();
  if (lower.startsWith('#') || lower.startsWith('/') || lower.startsWith('./') || lower.startsWith('../')) return true;
  if (lower.startsWith('http://') || lower.startsWith('https://') || lower.startsWith('mailto:')) return true;
  if (lower.startsWith('data:image/png;') || lower.startsWith('data:image/jpeg;') || lower.startsWith('data:image/gif;') || lower.startsWith('data:image/webp;')) return true;

  return false;
}

function sanitize(dirty: string, config: SanitizeConfig = {}): string {
  if (typeof document === 'undefined') return '';

  const allowedTags = new Set((config.ALLOWED_TAGS || []).map(tag => tag.toUpperCase()));
  const allowedAttrs = new Set((config.ALLOWED_ATTR || []).map(attr => attr.toLowerCase()));
  const template = document.createElement('template');
  template.innerHTML = dirty;

  const walker = document.createTreeWalker(template.content, NodeFilter.SHOW_ELEMENT);
  const elementsToRemove: Element[] = [];

  while (walker.nextNode()) {
    const element = walker.currentNode as Element;

    if (!allowedTags.has(element.tagName)) {
      elementsToRemove.push(element);
      continue;
    }

    for (const attr of Array.from(element.attributes)) {
      const name = attr.name;
      const lowerName = name.toLowerCase();

      if (lowerName.startsWith('on')) {
        element.removeAttribute(name);
        continue;
      }

      // The system-node fallback intentionally strips inline CSS. DOMPurify can
      // sanitize CSS more precisely; this local fallback stays conservative.
      if (lowerName === 'style') {
        element.removeAttribute(name);
        continue;
      }

      if (lowerName.startsWith('data-')) {
        if (!config.ALLOW_DATA_ATTR) element.removeAttribute(name);
        continue;
      }

      if (!allowedAttrs.has(lowerName)) {
        element.removeAttribute(name);
        continue;
      }

      if ((lowerName === 'href' || lowerName === 'src') && !isSafeUrl(attr.value)) {
        element.removeAttribute(name);
      }
    }
  }

  for (const element of elementsToRemove) {
    element.remove();
  }

  return template.innerHTML;
}

export type Config = SanitizeConfig;
export { sanitize };
export default { sanitize };
