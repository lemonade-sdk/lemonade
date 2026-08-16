import React, { useState } from 'react';
import { Icon } from './Icon';
import ModelViewer3D from './ModelViewer3D';
import { downloadBlob, glbUrlToStlBlob } from '../features/model3d/exportStl';
import { useI18n } from '../i18n';

interface Model3DResultProps {
  src: string;
  name?: string;
}

function baseName(name?: string): string {
  const cleaned = String(name || 'lemonade-model').replace(/\.(glb|gltf|stl)$/i, '').replace(/[^a-z0-9._-]+/gi, '-');
  return cleaned || 'lemonade-model';
}

const Model3DResult: React.FC<Model3DResultProps> = ({ src, name }) => {
  const { t } = useI18n('chat');
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
      setError(err instanceof Error ? err.message : t('model3d.exportFailed'));
    } finally {
      setExporting(false);
    }
  };

  return (
    <div className="message__model3d">
      <ModelViewer3D src={src} />
      <div className="message__model3d-actions">
        <a href={src} download={`${stem}.glb`} className="message__action message__action--primary">
          <Icon name="download" size={13} /> {t('model3d.downloadGlb')}
        </a>
        <button type="button" className="message__action" onClick={exportStl} disabled={exporting}>
          <Icon name="box" size={13} /> {exporting ? t('model3d.converting') : t('model3d.exportStl')}
        </button>
      </div>
      {error && <div className="message__model3d-error" role="alert">{error}</div>}
      <p className="message__model3d-note">{t('model3d.note')}</p>
    </div>
  );
};

export default Model3DResult;
