const ZERO_WIDTH = /[\u00ad\u200b\u200c\u200d\ufeff]/g;
const CONTROL_CHARS = /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g;

/**
 * Normalises extracted text without destroying structure: paragraph breaks and
 * Markdown headings survive, runs of whitespace and stray control characters
 * do not.
 */
export function cleanText(input: string): string {
  return input
    .replace(/\r\n?/g, "\n")
    .replace(ZERO_WIDTH, "")
    .replace(CONTROL_CHARS, " ")
    .replace(/[ \t]+/g, " ")
    .replace(/ *\n */g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}

export function countWords(text: string): number {
  const trimmed = text.trim();
  if (!trimmed) return 0;
  return trimmed.split(/\s+/).length;
}

/** True when a page yielded too little text to be a real text layer. */
export function looksScanned(text: string, minChars: number): boolean {
  return cleanText(text).length < minChars;
}

/**
 * Splits Markdown/plain text on ATX headings so each section keeps a usable
 * label. Text before the first heading becomes an untitled leading section.
 */
export function splitIntoSections(text: string): { title: string | null; text: string }[] {
  const lines = text.split("\n");
  const sections: { title: string | null; text: string }[] = [];
  let currentTitle: string | null = null;
  let buffer: string[] = [];

  const flush = () => {
    const body = buffer.join("\n").trim();
    if (body || currentTitle) sections.push({ title: currentTitle, text: body });
    buffer = [];
  };

  for (const line of lines) {
    const heading = /^(#{1,6})\s+(.*)$/.exec(line.trim());
    if (heading) {
      flush();
      currentTitle = heading[2]?.trim() ?? null;
      continue;
    }
    buffer.push(line);
  }
  flush();

  return sections.length > 0 ? sections : [{ title: null, text: text.trim() }];
}
