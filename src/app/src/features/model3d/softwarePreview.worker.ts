import { createGlbPreview, type SoftwarePreviewView } from './exportStl';

export type PreviewRequest = { type: 'load'; data: ArrayBuffer } | ({ type: 'render' } & SoftwarePreviewView);
export type PreviewResponse = { type: 'ready' } | { type: 'error' }
  | { type: 'frame'; width: number; height: number; pixels: ArrayBuffer };

// Keep the full-mesh rasterizer off the UI thread, without requiring
// OffscreenCanvas or WebGL in the worker.
const scope = self as unknown as {
  onmessage: ((event: MessageEvent<PreviewRequest>) => void) | null;
  postMessage(message: PreviewResponse, transfer?: Transferable[]): void;
};
let preview: ReturnType<typeof createGlbPreview> | null = null;
scope.onmessage = ({ data: request }) => {
  try {
    if (request.type === 'load') {
      preview = createGlbPreview(request.data);
      scope.postMessage({ type: 'ready' });
    } else if (preview) {
      const pixels = preview.render(request).buffer as ArrayBuffer;
      scope.postMessage({ type: 'frame', width: request.width, height: request.height, pixels }, [pixels]);
    }
  } catch {
    scope.postMessage({ type: 'error' });
  }
};
