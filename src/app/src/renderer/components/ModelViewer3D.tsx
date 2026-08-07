import React from 'react';
import '../vendor/model-viewer.min.js';

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
