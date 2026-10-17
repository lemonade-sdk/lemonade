import mermaid from 'mermaid';

let initialized = false;
let nextId = 0;
let renderTail: Promise<void> = Promise.resolve();

function ensureMermaidInit(): void {
  if (initialized) return;
  const options = {
    startOnLoad: false,
    theme: 'neutral',
    securityLevel: 'strict',
    suppressErrorRendering: true,
  } as const;
  mermaid.initialize(options);
  initialized = true;
}

export async function renderMermaidSvg(source: string): Promise<string> {
  // Mermaid 9 uses shared parser/config state; serialize rendering and validation.
  const previous = renderTail;
  let release: () => void = () => {};
  renderTail = new Promise<void>(resolve => { release = resolve; });
  await previous;
  const id = `mm-${Date.now()}-${++nextId}`;
  try {
    ensureMermaidInit();
    // The native Debian build currently aliases Mermaid to a fallback without parse().
    if (typeof mermaid.parse === 'function' && !(await mermaid.parse(source))) {
      throw new Error('Mermaid syntax error: parser rejected the diagram');
    }

    // Mermaid 9 returns a string; Mermaid 11 resolves to { svg, bindFunctions }.
    const result = (await mermaid.render(id, source)) as string | { svg: string };
    const svg = typeof result === 'string' ? result : result?.svg;
    if (typeof svg !== 'string' || !svg.trim()) {
      throw new Error('Mermaid returned no diagram markup');
    }
    return svg;
  } finally {
    // Mermaid 9 can leave an error SVG in <body> when render() throws.
    document.getElementById(`d${id}`)?.remove();
    document.getElementById(`i${id}`)?.remove();
    release();
  }
}

export function mermaidValidationAvailable(): boolean {
  return typeof mermaid.parse === 'function';
}
