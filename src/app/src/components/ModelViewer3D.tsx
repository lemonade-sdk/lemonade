import React, { useLayoutEffect, useRef, useState } from 'react';
import SoftwareModelViewer3D from './SoftwareModelViewer3D';

// Keep the viewer lazy-loaded with Model3DResult and use the pinned vendor bundle.
import '../vendor/model-viewer.min.js';

const ModelViewer = 'model-viewer' as unknown as React.FC<Record<string, unknown>>;

interface ModelViewer3DProps {
  src: string;
  alt?: string;
}

function supportsWebGL2(): boolean {
  try {
    const gl = document.createElement('canvas').getContext('webgl2', {
      alpha: true, antialias: true, preserveDrawingBuffer: true, powerPreference: 'high-performance',
    });
    if (!gl) return false;
    gl.getExtension('WEBGL_lose_context')?.loseContext();
    return true;
  } catch { return false; }
}

const ModelViewer3D: React.FC<ModelViewer3DProps> = ({ src, alt = '3D model preview' }) => {
  const [webgl] = useState(supportsWebGL2);
  const [failedSrc, setFailedSrc] = useState<string | null>(null);
  const ref = useRef<HTMLElement | null>(null);
  const native = webgl && failedSrc !== src;
  useLayoutEffect(() => {
    const element = ref.current;
    if (!native || !element) return;
    const onError = () => setFailedSrc(src);
    element.addEventListener('error', onError);
    return () => element.removeEventListener('error', onError);
  }, [native, src]);

  // model-viewer can report a loaded GLB without having a WebGL renderer.
  // In that case keep a usable geometry preview instead of an empty canvas.
  if (!native) return <SoftwareModelViewer3D key={src} src={src} alt={alt} />;
  return (
    <ModelViewer
      ref={ref}
      src={src}
      alt={alt}
      camera-controls
      auto-rotate
      shadow-intensity="1"
      exposure="1"
      interaction-prompt="auto"
      className="model3d-viewer"
    />
  );
};

export default ModelViewer3D;
