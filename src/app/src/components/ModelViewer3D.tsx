import React from 'react';

// Keep the viewer implementation identical to the proven GUI2 path. This file
// is only reached through React.lazy(Model3DResult), so the ~1 MiB vendor bundle
// stays out of the initial application boot while still being bundled and
// resolved by webpack instead of a fragile runtime <script> URL.
import '../vendor/model-viewer.min.js';

const ModelViewer = 'model-viewer' as unknown as React.FC<Record<string, unknown>>;

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
    interaction-prompt="auto"
    className="model3d-viewer"
  />
);

export default ModelViewer3D;
