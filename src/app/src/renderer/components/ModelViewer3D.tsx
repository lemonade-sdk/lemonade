import React from 'react';
// Side-effect import: registers the <model-viewer> custom element. Vendored as a
// single bundled file (three.js inlined) so it works in both the Tauri app and
// the Debian-packaged web-app without an npm dependency (see invariant #12).
import '../vendor/model-viewer.min.js';

// <model-viewer> is a custom element, not a typed JSX intrinsic; cast to render it.
const ModelViewer = 'model-viewer' as unknown as React.FC<any>;

interface ModelViewer3DProps {
  src: string;
  alt?: string;
}

const ModelViewer3D: React.FC<ModelViewer3DProps> = ({ src, alt = '3D model preview' }) => (
  <ModelViewer
    src={src}
    alt={alt}
    camera-controls
    auto-rotate
    shadow-intensity="1"
    exposure="1"
    style={{
      width: '100%',
      height: '100%',
      minHeight: '360px',
      background: '#1e1e1e',
      borderRadius: '8px',
    }}
  />
);

export default ModelViewer3D;
