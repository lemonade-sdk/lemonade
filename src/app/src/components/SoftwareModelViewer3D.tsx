import React, { useCallback, useEffect, useRef, useState } from 'react';
import { useI18n } from '../i18n';
import type { PreviewRequest, PreviewResponse } from '../features/model3d/softwarePreview.worker';

const SoftwareModelViewer3D: React.FC<{ src: string; alt: string }> = ({ src, alt }) => {
  const { t } = useI18n();
  const canvas = useRef<HTMLCanvasElement>(null);
  const worker = useRef<Worker | null>(null);
  const pending = useRef<Extract<PreviewRequest, { type: 'render' }> | null>(null);
  const rendering = useRef(false);
  const drag = useRef<{ id: number; x: number; y: number } | null>(null);
  const [ready, setReady] = useState(false);
  const [failed, setFailed] = useState(false);
  const [view, setView] = useState({ yaw: 0.5, pitch: 0.3, zoom: 1 });
  const rotate = (dx: number, dy: number) => setView(v => ({ ...v, yaw: v.yaw + dx, pitch: Math.max(-1.5, Math.min(1.5, v.pitch + dy)) }));
  const zoom = (factor: number) => setView(v => ({ ...v, zoom: Math.min(4, Math.max(0.25, v.zoom * factor)) }));

  // Only one frame in flight; fast pointer/resize events replace the queued view.
  const sendLatest = useCallback(() => {
    if (!worker.current || rendering.current || !pending.current) return;
    rendering.current = true;
    const request = pending.current;
    pending.current = null;
    worker.current.postMessage(request);
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    setReady(false); setFailed(false);
    rendering.current = false; pending.current = null;
    let renderer: Worker;
    try {
      renderer = new Worker(new URL('../features/model3d/softwarePreview.worker.ts', import.meta.url));
    } catch { setFailed(true); return; }
    worker.current = renderer;
    const fail = () => {
      if (controller.signal.aborted) return;
      setFailed(true);
      controller.abort(); renderer.terminate(); worker.current = null;
    };
    renderer.onerror = fail;
    renderer.onmessage = ({ data }: MessageEvent<PreviewResponse>) => {
      if (controller.signal.aborted) return;
      if (data.type === 'error') { fail(); return; }
      if (data.type === 'ready') { setReady(true); return; }
      const element = canvas.current;
      const ctx = element?.getContext('2d');
      if (!element || !ctx) { fail(); return; }
      if (element.width !== data.width || element.height !== data.height) {
        element.width = data.width; element.height = data.height;
      }
      ctx.putImageData(new ImageData(new Uint8ClampedArray(data.pixels), data.width, data.height), 0, 0);
      rendering.current = false;
      sendLatest();
    };
    void fetch(src, { signal: controller.signal }).then(response => {
      if (!response.ok) throw new Error(`GLB download failed (${response.status})`);
      return response.arrayBuffer();
    }).then(data => {
      if (!controller.signal.aborted) renderer.postMessage({ type: 'load', data } satisfies PreviewRequest, [data]);
    }).catch(fail);
    return () => { controller.abort(); renderer.terminate(); worker.current = null; };
  }, [src, sendLatest]);

  useEffect(() => {
    const element = canvas.current;
    if (!element || !ready || failed) return;
    const draw = () => {
      const w = Math.max(1, element.clientWidth); const h = Math.max(1, element.clientHeight);
      const scale = Math.min(1, 960 / Math.max(w, h));
      pending.current = { type: 'render', width: Math.max(1, Math.round(w * scale)), height: Math.max(1, Math.round(h * scale)), ...view };
      sendLatest();
    };
    draw();
    if (typeof ResizeObserver === 'undefined') {
      window.addEventListener('resize', draw);
      return () => window.removeEventListener('resize', draw);
    }
    const observer = new ResizeObserver(draw);
    observer.observe(element);
    return () => observer.disconnect();
  }, [ready, failed, view, sendLatest]);

  if (failed) return <div className="model3d-viewer model3d-viewer--error" role="alert">{t('3D preview unavailable. Download the GLB to view the model.')}</div>;
  if (!ready) return <div className="model3d-viewer model3d-viewer--loading" role="status">{t('Preparing 3D result…')}</div>;
  return <>
    <canvas ref={canvas} className="model3d-viewer" style={{ touchAction: 'none', cursor: 'grab' }} tabIndex={0} aria-label={alt}
      onPointerDown={e => { if (drag.current) return; e.currentTarget.setPointerCapture(e.pointerId); drag.current = { id: e.pointerId, x: e.clientX, y: e.clientY }; }}
      onPointerMove={e => { const p = drag.current; if (!p || p.id !== e.pointerId) return; rotate((e.clientX - p.x) * 0.01, (e.clientY - p.y) * 0.01); drag.current = { id: e.pointerId, x: e.clientX, y: e.clientY }; }}
      onLostPointerCapture={() => { drag.current = null; }}
      onKeyDown={e => {
        const moves: Record<string, [number, number]> = { ArrowLeft: [-0.15, 0], ArrowRight: [0.15, 0], ArrowUp: [0, -0.15], ArrowDown: [0, 0.15] };
        if (moves[e.key]) { e.preventDefault(); rotate(...moves[e.key]); }
        else if (e.key === '+' || e.key === '=') { e.preventDefault(); zoom(1.1); }
        else if (e.key === '-') { e.preventDefault(); zoom(1 / 1.1); }
      }} />
    <p className="message__model3d-note">{t('Software preview (geometry only). Drag to rotate; use + or - to zoom.')}</p>
  </>;
};

export default SoftwareModelViewer3D;
