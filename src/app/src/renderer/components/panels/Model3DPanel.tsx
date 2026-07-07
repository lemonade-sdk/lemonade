import React, { useState, useEffect, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { AppSettings } from '../../utils/appSettings';
import { serverFetch } from '../../utils/serverConfig';
import { ensureModelReady, DownloadAbortError } from '../../utils/backendInstaller';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import InferenceControls from '../InferenceControls';
import ImagePreviewList from '../ImagePreviewList';
import { ImageUploadIcon } from '../Icons';
import ModelViewer3D from '../ModelViewer3D';
import Combobox from '../Combobox';

const RES_OPTIONS = [
  '512 — ~3 GB VRAM, fast',
  '1024 — ~15 GB VRAM, sharp',
  '1536 — ~16+ GB VRAM, heavy',
];

const BG_OPTIONS = ['Auto matte (BiRefNet)', 'Plain background'];

const PROMPT_TAGS =
  'single subject, centered, whole object in frame, ' +
  'three-quarter view from slightly above showing the top and two sides, ' +
  'plain white background, even soft studio lighting, high detail, 3D asset render';

interface Model3DPanelProps {
  isBusy: boolean;
  isPreFlight: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
  appSettings: AppSettings | null;
}

const Model3DPanel: React.FC<Model3DPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality, runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData, downloadedModels } = useModels();

  const [sourceMode, setSourceMode] = useState<'image' | 'text'>('image');
  const [imageDataUrl, setImageDataUrl] = useState<string | null>(null);
  const [resLabel, setResLabel] = useState(RES_OPTIONS[0]);
  const [bgLabel, setBgLabel] = useState(BG_OPTIONS[0]);
  const [textPrompt, setTextPrompt] = useState('');
  const [imageModel, setImageModel] = useState('');
  const [generatingImage, setGeneratingImage] = useState(false);
  const [glbUrl, setGlbUrl] = useState<string | null>(null);

  const resolution = resLabel.match(/^\d+/)?.[0] ?? '512';
  const bgRemoval = bgLabel.startsWith('Auto') ? 'birefnet' : 'threshold';
  const imageModels = downloadedModels
    .filter((m) => m.info?.labels?.includes('image'))
    .map((m) => m.id);

  const fileInputRef = useRef<HTMLInputElement>(null);
  const glbUrlRef = useRef<string | null>(null);
  glbUrlRef.current = glbUrl;

  useEffect(() => () => { if (glbUrlRef.current) URL.revokeObjectURL(glbUrlRef.current); }, []);
  useEffect(() => {
    if (!imageModel && imageModels.length) setImageModel(imageModels[0]);
  }, [imageModels, imageModel]);

  const handlePickImage = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (ev) => setImageDataUrl(ev.target?.result as string);
    reader.readAsDataURL(file);
    e.target.value = '';
  };

  const reconstruct = async (b64: string, bg: string) => {
    const ready = await runPreFlight('model3d', { modelName: selectedModel, modelsData, onError: showError });
    if (!ready) return;
    const response = await serverFetch('/3d/generations', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ model: selectedModel, image: b64, resolution, bg_removal: bg }),
    });
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    const blob = await response.blob();
    if (glbUrlRef.current) URL.revokeObjectURL(glbUrlRef.current);
    setGlbUrl(URL.createObjectURL(blob));
  };

  const handleGenerate = async () => {
    if (isBusy || !selectedModel) return;

    try {
      if (sourceMode === 'image') {
        if (!imageDataUrl) return;
        const comma = imageDataUrl.indexOf(',');
        const b64 = comma >= 0 ? imageDataUrl.slice(comma + 1) : imageDataUrl;
        await reconstruct(b64, bgRemoval);
      } else {
        if (!textPrompt.trim() || !imageModel) return;
        setGeneratingImage(true);
        try {
          await ensureModelReady(imageModel, modelsData);
        } catch (e: any) {
          if (e instanceof DownloadAbortError) return;
          throw e;
        }
        const imgResp = await serverFetch('/images/generations', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            model: imageModel,
            prompt: `${textPrompt.trim()} -- ${PROMPT_TAGS}`,
            response_format: 'b64_json',
            n: 1,
            size: '1024x1024',
          }),
        });
        if (!imgResp.ok) throw new Error(`image generation failed (status ${imgResp.status})`);
        const imgData = await imgResp.json();
        const b64 = imgData.data?.[0]?.b64_json;
        if (!b64) throw new Error(imgData.error?.message || 'image generation returned no image');
        setGeneratingImage(false);
        await reconstruct(b64, bgRemoval);
      }
    } catch (error: any) {
      console.error('3D generation failed:', error);
      showError(`Failed to generate 3D model: ${error.message || 'Unknown error'}`);
    } finally {
      setGeneratingImage(false);
      reset();
    }
  };

  const noImageModels = sourceMode === 'text' && imageModels.length === 0;
  const sendDisabled = sourceMode === 'image'
    ? !imageDataUrl
    : (!textPrompt.trim() || noImageModels);

  return (
    <>
      <div className="chat-messages">
        {!glbUrl && !isBusy && <EmptyState title="Lemonade 3D Generator" />}
        {glbUrl && (
          <div className="chat-message" style={{ height: '440px', maxWidth: '100%' }}>
            <ModelViewer3D src={glbUrl} />
            <a href={glbUrl} download="model.glb" className="download-link" style={{ display: 'inline-block', marginTop: '6px' }}>
              Download .glb
            </a>
          </div>
        )}
        {generatingImage && (
          <div className="model-loading-indicator"><span className="model-loading-text">Rendering reference image...</span></div>
        )}
        {isPreFlight && activeModality === 'model3d' && (
          <div className="model-loading-indicator"><span className="model-loading-text">Loading 3D model...</span></div>
        )}
        {isInferring && activeModality === 'model3d' && (
          <div className="model-loading-indicator"><span className="model-loading-text">Reconstructing 3D mesh (this can take a couple of minutes)...</span></div>
        )}
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          <div className="model3d-source-toggle">
            <button className={`toggle-button${sourceMode === 'image' ? ' active' : ''}`} onClick={() => setSourceMode('image')} disabled={isBusy}>From image</button>
            <button className={`toggle-button${sourceMode === 'text' ? ' active' : ''}`} onClick={() => setSourceMode('text')} disabled={isBusy}>From text</button>
          </div>

          {sourceMode === 'image' ? (
            <>
              <ImagePreviewList
                images={imageDataUrl ? [imageDataUrl] : []}
                onRemove={() => setImageDataUrl(null)}
                altPrefix="Input"
              />
              <div className="chat-input model3d-hint" style={{ opacity: 0.7, pointerEvents: 'none' }}>
                {imageDataUrl ? 'Ready — press generate to reconstruct a 3D mesh.' : 'Attach an image to reconstruct into a 3D model.'}
              </div>
            </>
          ) : (
            <textarea
              className="chat-input"
              value={textPrompt}
              onChange={(e) => setTextPrompt(e.target.value)}
              placeholder={noImageModels ? 'Download an image-generation model to use text → 3D.' : 'Describe the object to model (e.g. an ornate wooden treasure chest)...'}
              rows={1}
              disabled={isBusy || noImageModels}
            />
          )}

          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleGenerate}
            sendDisabled={sendDisabled}
            sendTitle="Generate 3D"
            modelSelector={<ModelSelector disabled={isBusy} />}
            leftControls={
              <>
                {sourceMode === 'image' ? (
                  <>
                    <input
                      ref={fileInputRef}
                      type="file"
                      accept="image/png,image/jpeg,image/bmp,image/gif"
                      onChange={handlePickImage}
                      style={{ display: 'none' }}
                    />
                    <button
                      className="image-upload-button"
                      onClick={() => fileInputRef.current?.click()}
                      disabled={isBusy}
                      title={imageDataUrl ? 'Change image' : 'Upload image'}
                    >
                      <ImageUploadIcon />
                    </button>
                  </>
                ) : (
                  <div style={{ width: '190px' }} title="Image-generation model used to render the reference view">
                    <Combobox optionsList={imageModels} defaultValue={imageModel} onChangeFunc={setImageModel} placeholder="Image model" position="top" />
                  </div>
                )}
                <div style={{ width: '190px' }} title="Background removal applied before reconstruction">
                  <Combobox optionsList={BG_OPTIONS} defaultValue={bgLabel} onChangeFunc={setBgLabel} placeholder="Background" position="top" />
                </div>
                <div style={{ width: '210px' }} title="Cascade resolution (geometry detail vs VRAM/RAM cost)">
                  <Combobox optionsList={RES_OPTIONS} defaultValue={resLabel} onChangeFunc={setResLabel} placeholder="Cascade resolution" position="top" />
                </div>
              </>
            }
          />
        </div>
      </div>
    </>
  );
};

export default Model3DPanel;
