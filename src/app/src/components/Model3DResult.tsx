import React, { useState } from 'react';
import { Icon } from './Icon';
import ModelViewer3D from './ModelViewer3D';
import { downloadBlob, glbUrlToStlBlob } from '../features/model3d/exportStl';

interface Model3DResultProps {
  src: string;
  name?: string;
}

function baseName(name?: string): string {
  const cleaned = String(name || 'lemonade-model').replace(/\.(glb|gltf|stl)$/i, '').replace(/[^a-z0-9._-]+/gi, '-');
  return cleaned || 'lemonade-model';
}

const Model3DResult: React.FC<Model3DResultProps> = ({ src, name }) => {
  const [exporting, setExporting] = useState(false);
  const [error, setError] = useState('');
  const stem = baseName(name);

  const exportStl = async () => {
    if (exporting) return;
    setExporting(true);
    setError('');
    try {
      const blob = await glbUrlToStlBlob(src);
      downloadBlob(blob, `${stem}.stl`);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'STL export failed.');
    } finally {
      setExporting(false);
    }
  };

  return (
    <div className="message__model3d">
      <ModelViewer3D src={src} />
      <div className="message__model3d-actions">
        <a href={src} download={`${stem}.glb`} className="message__action message__action--primary">
          <Icon name="download" size={13} /> Download GLB
        </a>
        <button type="button" className="message__action" onClick={exportStl} disabled={exporting}>
          <Icon name="box" size={13} /> {exporting ? 'Converting…' : 'Export STL'}
        </button>
      </div>
      {error && <div className="message__model3d-error" role="alert">{error}</div>}
      <p className="message__model3d-note">STL contains mesh geometry only; GLB keeps materials and textures.</p>
    </div>
  );
};

export default Model3DResult;
