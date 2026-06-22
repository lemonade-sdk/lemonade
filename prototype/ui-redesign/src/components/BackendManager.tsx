import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { friendlyErrorMessage } from '../api';
import {
  DEFAULT_PRESET,
  PRESET_STORE_EVENT,
  Capability,
  Preset,
  STARTERS,
  loadBackendApplied,
  loadUserPresets,
  presetParamPreviewLines,
  presetSupportsCapability,
  saveBackendApplied,
} from '../presetStore';
import { Icon, PresetIcon } from './Icon';
import { DownloadListItem, downloadStore, isDownloadActive } from '../features/downloadManager/downloadStore';

/* ── Types matching /api/v1/system-info response ─────────── */

interface DeviceInfo {
  name: string;
  available: boolean;
  family?: string;
  cores?: number;
  threads?: number;
  vram_gb?: number;
  tops_max_int?: number;
}

interface BackendInfo {
  devices?: string[];
  state: 'installed' | 'installable' | 'unsupported' | 'update_required' | 'update_available' | 'action_required';
  version: string;
  message: string;
  action: string;
  release_url?: string;
  download_filename?: string;
  can_uninstall?: boolean;
}

interface RecipeInfo {
  default_backend: string;
  backends: Record<string, BackendInfo>;
}

interface SystemInfoData {
  'OS Version'?: string;
  os_version?: string;
  lemonade_version?: string;
  version?: string;
  devices: {
    cpu?: DeviceInfo;
    amd_gpu?: DeviceInfo[];
    amd_dgpu?: DeviceInfo[];
    amd_igpu?: DeviceInfo;
    nvidia_gpu?: DeviceInfo[];
    amd_npu?: DeviceInfo;
    npu?: DeviceInfo;
    metal?: DeviceInfo;
  };
  recipes: Record<string, RecipeInfo>;
}

/* ── Constants ─────────────────────────────────────────────── */

/** User-facing labels for recipes */
const RECIPE_LABELS: Record<string, string> = {
  llamacpp:       'llama.cpp',
  whispercpp:     'whisper.cpp',
  moonshine:      'Moonshine',
  'sd-cpp':       'stable-diffusion.cpp',
  kokoro:         'Kokoro TTS',
  flm:            'FastFlowLM',
  'ryzenai-llm':  'RyzenAI',
  vllm:           'vLLM',
};

/** Recipe → capability column for the matrix */
const RECIPE_CAPABILITY: Record<string, string> = {
  llamacpp:       'LLM',
  whispercpp:     'Audio',
  moonshine:      'Audio',
  'sd-cpp':       'Image',
  kokoro:         'TTS',
  flm:            'LLM',
  'ryzenai-llm':  'LLM',
  vllm:           'LLM',
};

/** Device display order */
const DEVICE_ORDER = ['cpu', 'nvidia_gpu', 'amd_gpu', 'metal', 'amd_npu', 'gpu', 'accelerator', 'unknown'] as const;
type DeviceKey = typeof DEVICE_ORDER[number];

const DEVICE_LABELS: Record<DeviceKey, string> = {
  cpu:         'CPU',
  amd_gpu:     'GPU (AMD)',
  nvidia_gpu:  'GPU (NVIDIA / CUDA)',
  metal:       'GPU (Metal)',
  amd_npu:     'NPU',
  gpu:         'GPU',
  accelerator: 'Accelerator',
  unknown:     'Other device',
};

/** Backend → fallback row when the server does not expose BackendInfo.devices */
const BACKEND_DEVICE: Record<string, DeviceKey> = {
  cpu:            'cpu',
  system:         'cpu',
  vulkan:         'gpu',
  directml:       'gpu',
  dml:            'gpu',
  cuda:           'nvidia_gpu',
  cuda11:         'nvidia_gpu',
  cuda12:         'nvidia_gpu',
  'cuda-11':      'nvidia_gpu',
  'cuda-12':      'nvidia_gpu',
  nvidia:         'nvidia_gpu',
  rocm:           'amd_gpu',
  'rocm-stable':  'amd_gpu',
  'rocm-nightly': 'amd_gpu',
  metal:          'metal',
  npu:            'amd_npu',
  ryzenai:        'amd_npu',
};

/** Capability columns */
const CAPABILITY_COLS = ['LLM', 'Audio', 'Image', 'TTS'] as const;

type CapabilityCol = typeof CAPABILITY_COLS[number];
type CellEntry = { recipe: string; backend: string; info: BackendInfo };

function backendKey(recipe: string, backend: string): string {
  return `${recipe}:${backend}`;
}

function backendDownloadId(recipe: string, backend: string): string {
  return `backend:${backendKey(recipe, backend)}`;
}

function backendDownloadName(recipe: string, backend: string): string {
  return backendKey(recipe, backend);
}

function backendDownloadMatches(download: DownloadListItem, recipe: string, backend: string): boolean {
  const name = backendDownloadName(recipe, backend);
  return download.downloadType === 'backend'
    && (download.id === backendDownloadId(recipe, backend) || download.modelName === name);
}

function backendProgressPercent(download: DownloadListItem): number {
  return Math.max(0, Math.min(100, Number.isFinite(download.percent) ? download.percent : 0));
}

function backendCapabilitiesForRecipe(recipe: string): Capability[] {
  switch (recipe) {
    case 'sd-cpp': return ['image'];
    case 'whispercpp':
    case 'moonshine': return ['transcription'];
    case 'kokoro': return ['tts'];
    case 'llamacpp':
    case 'flm':
    case 'ryzenai-llm':
    case 'vllm':
    default: return ['chat', 'code', 'vision', 'omni'];
  }
}

function presetCompatibleWithBackend(preset: Preset, key: string): boolean {
  if (!key) return true;
  const [recipe] = key.split(':');
  return backendCapabilitiesForRecipe(recipe).some(cap => presetSupportsCapability(preset, cap));
}


function backendLabelFromKey(key: string): string {
  const [recipe, backend] = key.split(':');
  return `${RECIPE_LABELS[recipe] || recipe} · ${backend || 'auto'}`;
}

/* ── Helpers ─────────────────────────────────────────────── */

function stateBadge(state: BackendInfo['state']): { label: string; cls: string } {
  switch (state) {
    case 'installed':        return { label: 'Installed',         cls: 'cell__badge--ok' };
    case 'installable':      return { label: 'Available',         cls: 'cell__badge--available' };
    case 'update_required':  return { label: 'Update required',   cls: 'cell__badge--warn' };
    case 'update_available': return { label: 'Update available',  cls: 'cell__badge--warn' };
    case 'action_required':  return { label: 'Action required',   cls: 'cell__badge--warn' };
    case 'unsupported':      return { label: 'Unsupported',       cls: 'cell__badge--off' };
    default:                 return { label: state,                cls: '' };
  }
}

function uniq<T>(values: T[]): T[] {
  return Array.from(new Set(values));
}

function asArray<T>(value: T | T[] | undefined | null): T[] {
  if (!value) return [];
  return Array.isArray(value) ? value : [value];
}

function cleanString(value: unknown): string {
  const s = String(value || '').trim();
  return s && s.toLowerCase() !== 'unknown' ? s : '';
}

function lemonadeVersion(info: SystemInfoData | null): string {
  return cleanString(info?.lemonade_version)
    || cleanString(info?.version)
    || cleanString(api.healthData?.version)
    || 'unknown';
}

function osVersion(info: SystemInfoData | null): string {
  return cleanString(info?.['OS Version'])
    || cleanString(info?.os_version)
    || 'OS unknown';
}

function amdGpuDevices(info: SystemInfoData | null): DeviceInfo[] {
  if (!info?.devices) return [];
  return [
    ...asArray(info.devices.amd_gpu),
    ...asArray(info.devices.amd_dgpu),
    ...asArray(info.devices.amd_igpu),
  ];
}

function amdNpuDevice(info: SystemInfoData | null): DeviceInfo | undefined {
  return info?.devices?.amd_npu || info?.devices?.npu;
}

function normalizeDeviceToken(token: string): DeviceKey {
  const t = token.toLowerCase().replace(/[^a-z0-9]+/g, '');
  if (!t || t === 'unknown') return 'unknown';
  if (t.includes('cuda') || t.includes('nvidia')) return 'nvidia_gpu';
  if (t.includes('rocm') || t.includes('amd') || t.includes('radeon')) return 'amd_gpu';
  if (t.includes('metal') || t.includes('apple')) return 'metal';
  if (t.includes('npu') || t.includes('ryzenai')) return 'amd_npu';
  if (t.includes('cpu') || t.includes('system')) return 'cpu';
  if (t.includes('gpu') || t.includes('vulkan') || t.includes('directml') || t === 'dml') return 'gpu';
  if (t.includes('accelerator')) return 'accelerator';
  return 'unknown';
}

function devicesForBackend(recipe: string, backend: string, info: BackendInfo): DeviceKey[] {
  // FastFlowLM is the NPU path in Lemonade. Some older system-info
  // payloads report its backend token as a generic GPU/DirectML backend,
  // which placed FLM in the wrong matrix row. Keep the prototype UI
  // aligned with the actual runtime target.
  if (recipe === 'flm') return ['amd_npu'];

  const fromServer = Array.isArray(info.devices)
    ? info.devices.map(normalizeDeviceToken).filter(Boolean)
    : [];
  if (fromServer.length > 0) return uniq(fromServer);
  return [BACKEND_DEVICE[backend] || normalizeDeviceToken(backend)];
}

function canShowUninstall(info: BackendInfo): boolean {
  if (info.can_uninstall === false) return false;
  return info.state === 'installed'
    || info.state === 'update_required'
    || info.state === 'update_available';
}

/* ── Component ─────────────────────────────────────────────── */

const BackendManager: React.FC = () => {
  const [sysInfo, setSysInfo] = useState<SystemInfoData | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [showTech, setShowTech] = useState(false);
  const [installing, setInstalling] = useState<string | null>(null); // "recipe:backend"
  const [toastMsg, setToastMsg] = useState<string | null>(null);
  const [userPresets, setUserPresets] = useState<Preset[]>(loadUserPresets);
  const [backendPresets, setBackendPresets] = useState<Record<string, string>>(loadBackendApplied);
  const [presetRailCollapsed, setPresetRailCollapsed] = useState(false);
  const [selectedBackendKey, setSelectedBackendKey] = useState('');
  const [selectedRailPresetId, setSelectedRailPresetId] = useState(DEFAULT_PRESET.id);
  const [presetRailHovered, setPresetRailHovered] = useState(false);
  const [hoveredRailPresetId, setHoveredRailPresetId] = useState<string | null>(null);
  const [presetNotice, setPresetNotice] = useState<string | null>(null);
  const [downloadItems, setDownloadItems] = useState<DownloadListItem[]>(() => downloadStore.snapshot());
  const terminalBackendRefreshRef = useRef<Set<string>>(new Set());

  useEffect(() => {
    const reloadPresetState = () => {
      setUserPresets(loadUserPresets());
      setBackendPresets(loadBackendApplied());
    };
    window.addEventListener(PRESET_STORE_EVENT, reloadPresetState);
    return () => window.removeEventListener(PRESET_STORE_EVENT, reloadPresetState);
  }, []);

  /* ── Fetch system-info ────────────────────────────────── */

  const fetchInfo = useCallback(async (showSpinner = true) => {
    try {
      if (showSpinner) setLoading(true);
      setError(null);
      if (!api.healthData) await api.health().catch(() => null);
      const data = await api.systemInfo() as unknown as SystemInfoData;
      setSysInfo(data);
    } catch (err) {
      setError(friendlyErrorMessage(err));
    } finally {
      if (showSpinner) setLoading(false);
    }
  }, []);

  useEffect(() => { void fetchInfo(); }, [fetchInfo]);

  useEffect(() => api.onModelsChanged(() => { void fetchInfo(false); }), [fetchInfo]);

  useEffect(() => downloadStore.subscribe((items) => {
    setDownloadItems(items);
    for (const item of items) {
      if (item.downloadType !== 'backend') continue;
      if (isDownloadActive(item)) continue;
      if (item.status !== 'completed' && item.status !== 'error' && item.status !== 'cancelled') continue;
      const refreshKey = `${item.id}:${item.status}:${item.terminalAt || item.updatedAt}`;
      if (terminalBackendRefreshRef.current.has(refreshKey)) continue;
      terminalBackendRefreshRef.current.add(refreshKey);
      void fetchInfo(false);
    }
  }), [fetchInfo]);

  /* ── Actions ──────────────────────────────────────────── */

  const toast = useCallback((msg: string) => {
    setToastMsg(msg);
    window.setTimeout(() => setToastMsg(null), 3500);
  }, []);

  const handleInstall = useCallback(async (recipe: string, backend: string, isUpdate = false, skipConfirm = false) => {
    const key = backendKey(recipe, backend);
    const actionLabel = isUpdate ? 'Updating' : 'Installing';
    const doneLabel = isUpdate ? 'updated' : 'installed';
    if (!skipConfirm) {
      const verb = isUpdate ? 'update' : 'install';
      const ok = window.confirm(`${actionLabel} ${RECIPE_LABELS[recipe] || recipe} · ${backend} can change local Lemonade runtime files. Continue with this ${verb}?`);
      if (!ok) return;
    }
    setInstalling(key);
    toast(`${actionLabel} ${RECIPE_LABELS[recipe] || recipe} · ${backend}…`);
    const downloadName = backendDownloadName(recipe, backend);
    downloadStore.markLocal(downloadName, 'downloading', 'backend');
    try {
      await api.installBackend(recipe, backend, {
        onProgress: (d) => {
          const percent = typeof d.percent === 'number' ? d.percent : undefined;
          downloadStore.upsertFromPull(downloadName, {
            ...d,
            id: backendDownloadId(recipe, backend),
            type: 'backend',
            name: downloadName,
            status: 'downloading',
            percent: percent ?? d.percent,
          }, 'backend');
          if (d.percent != null) {
            setToastMsg(`${actionLabel} ${RECIPE_LABELS[recipe] || recipe} · ${backend}… ${d.percent}%`);
          }
        },
        onComplete: async () => {
          downloadStore.upsertFromPull(downloadName, {
            id: backendDownloadId(recipe, backend),
            type: 'backend',
            name: downloadName,
            status: 'completed',
            complete: true,
            percent: 100,
          }, 'backend');
          toast(`${RECIPE_LABELS[recipe] || recipe} · ${backend} ${doneLabel}`);
          setInstalling(null);
          try {
            const fresh = await api.systemInfo() as unknown as SystemInfoData;
            setSysInfo(fresh);
            if (isUpdate && fresh?.recipes?.[recipe]?.backends?.[backend]) {
              const newState = fresh.recipes[recipe].backends[backend].state;
              if (newState === 'update_required' || newState === 'update_available') {
                toast(`${RECIPE_LABELS[recipe] || recipe} · ${backend} still needs update — the existing binary may need to be removed manually`);
              }
            }
          } catch {
            void fetchInfo(false);
          }
        },
        onError: (err) => {
          downloadStore.upsertFromPull(downloadName, {
            id: backendDownloadId(recipe, backend),
            type: 'backend',
            name: downloadName,
            status: 'error',
            error: friendlyErrorMessage(err),
          }, 'backend');
          toast(`${actionLabel} failed: ${friendlyErrorMessage(err)}`);
          setInstalling(null);
        },
      });
      await fetchInfo(false);
    } catch (err) {
      const message = friendlyErrorMessage(err);
      downloadStore.upsertFromPull(downloadName, {
        id: backendDownloadId(recipe, backend),
        type: 'backend',
        name: downloadName,
        status: 'error',
        error: message,
      }, 'backend');
      toast(`${actionLabel} failed: ${message}`);
      setInstalling(null);
      void fetchInfo(false);
    }
  }, [fetchInfo, toast]);

  const handleUninstall = useCallback(async (recipe: string, backend: string) => {
    const ok = window.confirm(`Uninstall ${RECIPE_LABELS[recipe] || recipe} · ${backend}? This removes local backend runtime files.`);
    if (!ok) return;
    try {
      setInstalling(backendKey(recipe, backend));
      await api.uninstallBackend(recipe, backend);
      toast(`${RECIPE_LABELS[recipe] || recipe} · ${backend} uninstalled`);
      void fetchInfo(false);
    } catch (err) {
      toast(`Uninstall failed: ${friendlyErrorMessage(err)}`);
    } finally {
      setInstalling(null);
    }
  }, [fetchInfo, toast]);

  const handleUpdateAll = useCallback(async () => {
    if (!sysInfo?.recipes) return;
    const updates: { recipe: string; backend: string }[] = [];
    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      for (const [backend, bInfo] of Object.entries(recipeInfo.backends)) {
        if (bInfo.state === 'update_required' || bInfo.state === 'update_available') updates.push({ recipe, backend });
      }
    }
    if (updates.length === 0) return;
    const ok = window.confirm(`Update ${updates.length} backend${updates.length > 1 ? 's' : ''}? This can change local Lemonade runtime files.`);
    if (!ok) return;
    for (const { recipe, backend } of updates) {
      await handleInstall(recipe, backend, true, true);
    }
  }, [sysInfo, handleInstall]);

  const handleAction = useCallback((url: string) => {
    window.open(url, '_blank', 'noopener,noreferrer');
  }, []);

  /* ── Build the matrix ─────────────────────────────────── */

  const detectedDevices = useMemo(() => {
    if (!sysInfo) return [] as DeviceKey[];
    const devs: DeviceKey[] = [];
    if (sysInfo.devices.cpu?.available) devs.push('cpu');
    if ((sysInfo.devices.nvidia_gpu || []).some(g => g.available)) devs.push('nvidia_gpu');
    if (amdGpuDevices(sysInfo).some(g => g.available)) devs.push('amd_gpu');
    if (sysInfo.devices.metal?.available) devs.push('metal');
    if (amdNpuDevice(sysInfo)?.available) devs.push('amd_npu');
    return devs;
  }, [sysInfo]);

  const matrixCells = useMemo(() => {
    if (!sysInfo?.recipes) return new Map<string, CellEntry[]>();
    const cells = new Map<string, CellEntry[]>();

    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      const cap = (RECIPE_CAPABILITY[recipe] || 'LLM') as CapabilityCol;
      for (const [backend, backendInfo] of Object.entries(recipeInfo.backends)) {
        for (const device of devicesForBackend(recipe, backend, backendInfo)) {
          const key = `${device}:${cap}`;
          if (!cells.has(key)) cells.set(key, []);
          cells.get(key)!.push({ recipe, backend, info: backendInfo });
        }
      }
    }
    return cells;
  }, [sysInfo]);

  const matrixRows = useMemo(() => {
    const referenced = new Set<DeviceKey>();
    for (const key of matrixCells.keys()) {
      const device = key.split(':')[0] as DeviceKey;
      if (DEVICE_ORDER.includes(device)) referenced.add(device);
    }
    return DEVICE_ORDER.filter(d => detectedDevices.includes(d) || referenced.has(d));
  }, [detectedDevices, matrixCells]);

  const updatesAvailable = useMemo(() => {
    if (!sysInfo?.recipes) return 0;
    let count = 0;
    for (const recipeInfo of Object.values(sysInfo.recipes)) {
      for (const bInfo of Object.values(recipeInfo.backends)) {
        if (bInfo.state === 'update_required' || bInfo.state === 'update_available') count++;
      }
    }
    return count;
  }, [sysInfo]);

  const allPresets = useMemo(() => [DEFAULT_PRESET, ...STARTERS, ...userPresets], [userPresets]);

  const activePresetForBackendKey = useCallback((key: string): Preset => {
    const presetId = backendPresets[key] || DEFAULT_PRESET.id;
    const preset = allPresets.find(p => p.id === presetId) || DEFAULT_PRESET;
    return presetCompatibleWithBackend(preset, key) ? preset : DEFAULT_PRESET;
  }, [allPresets, backendPresets]);

  const selectedRailPreset = allPresets.find(p => p.id === selectedRailPresetId) || DEFAULT_PRESET;
  const selectedBackendPreset = selectedBackendKey ? activePresetForBackendKey(selectedBackendKey) : null;
  const railSummaryPreset = selectedBackendPreset || selectedRailPreset;
  const highlightedPresetId = presetRailHovered ? (hoveredRailPresetId || railSummaryPreset.id) : null;

  const allBackendKeys = useMemo(() => {
    if (!sysInfo?.recipes) return [] as string[];
    const keys: string[] = [];
    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      for (const backend of Object.keys(recipeInfo.backends)) keys.push(backendKey(recipe, backend));
    }
    return keys.sort((a, b) => backendLabelFromKey(a).localeCompare(backendLabelFromKey(b)));
  }, [sysInfo]);

  const backendsUsingSelectedPreset = useMemo(
    () => allBackendKeys.filter(key => activePresetForBackendKey(key).id === railSummaryPreset.id),
    [allBackendKeys, activePresetForBackendKey, railSummaryPreset.id],
  );

  const handlePresetRailPick = useCallback((preset: Preset) => {
    setSelectedRailPresetId(preset.id);
    if (!selectedBackendKey) return;
    if (!presetCompatibleWithBackend(preset, selectedBackendKey)) {
      setPresetNotice(`“${preset.name}” does not apply to ${backendLabelFromKey(selectedBackendKey)}.`);
      window.setTimeout(() => setPresetNotice(null), 2800);
      return;
    }
    setBackendPresets(prev => {
      const next = { ...prev };
      if (preset.id === DEFAULT_PRESET.id) delete next[selectedBackendKey];
      else next[selectedBackendKey] = preset.id;
      saveBackendApplied(next);
      return next;
    });
    setPresetNotice(`${backendLabelFromKey(selectedBackendKey)} → ${preset.name}`);
    window.setTimeout(() => setPresetNotice(null), 2200);
  }, [selectedBackendKey]);

  /* ── Device detail row ────────────────────────────────── */

  const deviceDetail = useCallback((deviceKey: DeviceKey): string => {
    if (!sysInfo) return '';
    switch (deviceKey) {
      case 'cpu': return sysInfo.devices.cpu?.name || '';
      case 'amd_gpu': return amdGpuDevices(sysInfo).map(g => g.name).filter(Boolean).join(', ') || '';
      case 'nvidia_gpu': return (sysInfo.devices.nvidia_gpu || []).map(g => g.name).join(', ') || '';
      case 'metal': return sysInfo.devices.metal?.name || '';
      case 'amd_npu': return amdNpuDevice(sysInfo)?.name || '';
      default: return '';
    }
  }, [sysInfo]);

  const renderPresetRail = () => (
    <aside
      className={`context-rail context-rail--presets${presetRailCollapsed ? ' is-collapsed' : ''}`}
      aria-label="Backend preset rail"
      onMouseEnter={() => setPresetRailHovered(true)}
      onMouseLeave={() => { setPresetRailHovered(false); setHoveredRailPresetId(null); }}
    >
      <div className="context-rail__head">
        <button type="button" className="context-rail__toggle" onClick={() => setPresetRailCollapsed(v => !v)} aria-label="Toggle backend preset rail">☰</button>
        <div className="context-rail__title-wrap">
          <span className="context-rail__eyebrow">By backend</span>
          <strong className="context-rail__title">{selectedBackendKey ? backendLabelFromKey(selectedBackendKey) : 'Presets'}</strong>
        </div>
      </div>
      <div className="context-rail__body">
        <div className="preset-rail-summary">
          <span className="preset-rail-summary__label">Selected preset</span>
          <strong><PresetIcon preset={railSummaryPreset} /> {railSummaryPreset.name}</strong>
          <span>{backendsUsingSelectedPreset.length} backend{backendsUsingSelectedPreset.length === 1 ? '' : 's'} assigned</span>
          <span className="preset-param-lines">{presetParamPreviewLines(railSummaryPreset).map(line => <span key={line}>{line}</span>)}</span>
        </div>
        <p className="context-rail__hint">
          {selectedBackendKey ? 'Click a preset to connect it with this backend baseline.' : 'Hover or pick a preset to outline matching backends.'}
        </p>
        <div className="preset-rail-list">
          {allPresets.map(preset => {
            const isActive = selectedBackendKey ? selectedBackendPreset?.id === preset.id : selectedRailPreset.id === preset.id;
            const disabled = Boolean(selectedBackendKey && !presetCompatibleWithBackend(preset, selectedBackendKey));
            return (
              <button
                key={preset.id}
                type="button"
                className={`preset-rail-card${isActive ? ' is-active' : ''}${disabled ? ' is-disabled' : ''}`}
                onClick={() => handlePresetRailPick(preset)}
                onMouseEnter={() => setHoveredRailPresetId(preset.id)}
                onFocus={() => setHoveredRailPresetId(preset.id)}
                onBlur={() => setHoveredRailPresetId(null)}
                title={disabled ? 'Incompatible with selected backend capability' : preset.description}
              >
                <span className="preset-rail-card__icon">{isActive ? <Icon name="check" size={13} /> : <PresetIcon preset={preset} />}</span>
                <span className="preset-rail-card__text">
                  <strong>{preset.name}</strong>
                  <span className="preset-rail-card__params preset-param-lines">{presetParamPreviewLines(preset).map(line => <span key={line}>{line}</span>)}</span>
                </span>
              </button>
            );
          })}
        </div>
        {selectedBackendKey && selectedBackendPreset && (
          <div className="preset-rail-summary preset-rail-summary--backend">
            <span className="preset-rail-summary__label">Selected backend</span>
            <strong>{backendLabelFromKey(selectedBackendKey)}</strong>
            <span><PresetIcon preset={selectedBackendPreset} /> {selectedBackendPreset.name}</span>
          </div>
        )}
        {/* #2351: always-present polite live region for preset assignment notices (NVDA) */}
        <div role="status" aria-live="polite" aria-atomic="true" className="sr-only" data-backends-preset-notice-live>
          {presetNotice || ''}
        </div>
        {presetNotice && <div className="context-rail__notice">{presetNotice}</div>}
      </div>
    </aside>
  );


  /* ── Render ───────────────────────────────────────────── */

  if (loading && !sysInfo) {
    return (
      <section className="backends" data-view="backends">
        <div className="backends__head">
          <div className="backends__title"><h1>Backends</h1></div>
        </div>
        <div style={{ display: 'flex', justifyContent: 'center', padding: 'var(--space-8)' }}>
          <div className="hf-zone__spinner" />
        </div>
      </section>
    );
  }

  return (
    <section className={`backends backends--with-rail${showTech ? ' show-tech' : ''}${presetRailCollapsed ? ' context-rail-collapsed' : ''}`} data-view="backends">
      {renderPresetRail()}
      <div className="backends__main">
      <div className="backends__head">
        <div className="backends__title">
          <h1>Backends</h1>
          <label className="backends__toggle">
            <input
              type="checkbox"
              checked={showTech}
              onChange={e => setShowTech(e.target.checked)}
            />
            <span>Show technical details</span>
          </label>
        </div>

        {error && (
          <div className="banner banner--error" data-backends-error>
            <span className="banner__icon" aria-hidden="true"><Icon name="alert" size={16} /></span>
            <span className="banner__text">Could not load backend system info: {error}</span>
            <button className="banner__action" onClick={() => void fetchInfo()} disabled={loading}>Retry</button>
          </div>
        )}

        {updatesAvailable > 0 && (
          <div className="banner banner--warn" data-backends-banner>
            <span className="banner__icon" aria-hidden="true"><Icon name="alert" size={16} /></span>
            <span className="banner__text" data-backends-banner-text>
              {updatesAvailable} backend update{updatesAvailable > 1 ? 's' : ''} available
            </span>
            <button className="banner__action" data-backends-banner-action
              onClick={handleUpdateAll}
              disabled={installing !== null}>
              {installing ? 'Updating…' : 'Update all'}
            </button>
          </div>
        )}

        {sysInfo && (
          <div className="backends__summary">
            <span className="backends__version">
              Lemonade {lemonadeVersion(sysInfo)}
            </span>
            <span className="backends__os">{osVersion(sysInfo)}</span>
          </div>
        )}
      </div>

      {matrixRows.length === 0 ? (
        <div className="matrix matrix--empty" data-backends-matrix-empty>
          <div className="hf-zone__empty">
            <Icon name="box" size={20} />
            <span>No backend/device data is available for this Lemonade server yet.</span>
          </div>
        </div>
      ) : (
        <div className="matrix" data-backends-matrix>
          <table>
            <thead>
              <tr>
                <th scope="col">Device</th>
                {CAPABILITY_COLS.map(c => (
                  <th scope="col" key={c}>{c}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {matrixRows.map(deviceKey => (
                <tr key={deviceKey}>
                  <th scope="row">
                    {DEVICE_LABELS[deviceKey]}
                    {showTech && (
                      <div className="cell__device-detail">{deviceDetail(deviceKey) || 'reported by backend metadata'}</div>
                    )}
                  </th>
                  {CAPABILITY_COLS.map(cap => {
                    const key = `${deviceKey}:${cap}`;
                    const entries = matrixCells.get(key);
                    if (!entries || entries.length === 0) {
                      return (
                        <td className="matrix__empty" key={cap}>
                          <span aria-hidden="true">—</span>
                        </td>
                      );
                    }
                    return (
                      <td key={cap}>
                        {entries.map(({ recipe, backend, info }) => {
                          const badge = stateBadge(info.state);
                          const cellKey = backendKey(recipe, backend);
                          const isInstalling = installing === cellKey;
                          const backendDownload = downloadItems.find(download => backendDownloadMatches(download, recipe, backend));
                          const showBackendProgress = Boolean(backendDownload && (isDownloadActive(backendDownload) || backendDownload.status === 'paused'));
                          const activePreset = activePresetForBackendKey(cellKey);
                          const isSelectedBackend = selectedBackendKey === cellKey;
                          const isPresetHighlighted = Boolean(highlightedPresetId && activePreset.id === highlightedPresetId);
                          return (
                            <div className={`cell cell--selectable${isSelectedBackend ? ' is-selected' : ''}${isPresetHighlighted ? ' cell--preset-highlight' : ''}`} key={`${recipe}-${backend}`}
                              data-cell={cellKey}>
                              {/* #2343: overlay button provides keyboard + SR selection; covers the full cell area */}
                              <button
                                type="button"
                                className="cell__select-btn"
                                aria-pressed={isSelectedBackend}
                                aria-label={`${RECIPE_LABELS[recipe] || recipe} (${backend}), ${badge.label}`}
                                onClick={() => setSelectedBackendKey(current => current === cellKey ? '' : cellKey)}
                              />
                              <span className="cell__name">
                                {RECIPE_LABELS[recipe] || recipe}
                                {backend !== 'cpu' && backend !== 'npu' && ` · ${backend}`}
                              </span>
                              <span className={`cell__badge ${badge.cls}`}>{badge.label}</span>
                              {showTech && info.version && <span className="cell__sha">{info.version}</span>}
                              <span className="cell__preset"><PresetIcon preset={activePreset} /> {activePreset.name}</span>
                              {showTech && info.message && <span className="cell__message">{info.message}</span>}
                              <div className="cell__actions" onClick={e => e.stopPropagation()}>
                                {(info.state === 'installable') && (
                                  <button
                                    className="cell__swap"
                                    disabled={isInstalling}
                                    aria-label={`Install ${RECIPE_LABELS[recipe] || recipe} (${backend})`}
                                    onClick={() => handleInstall(recipe, backend)}>
                                    {isInstalling ? 'Installing…' : 'Install'}
                                  </button>
                                )}
                                {(info.state === 'update_required' || info.state === 'update_available') && (
                                  <button
                                    className="cell__swap"
                                    disabled={isInstalling}
                                    aria-label={`Update ${RECIPE_LABELS[recipe] || recipe} (${backend})`}
                                    onClick={() => handleInstall(recipe, backend, true)}>
                                    {isInstalling ? 'Updating…' : 'Update'}
                                  </button>
                                )}
                                {info.state === 'action_required' && info.action && (
                                  <button
                                    className="cell__swap"
                                    aria-label={`Setup guide for ${RECIPE_LABELS[recipe] || recipe} (${backend})`}
                                    onClick={() => handleAction(info.action)}>
                                    Setup guide ▸
                                  </button>
                                )}
                                {canShowUninstall(info) && (
                                  <button
                                    className="cell__swap cell__swap--danger"
                                    disabled={isInstalling}
                                    aria-label={`Uninstall ${RECIPE_LABELS[recipe] || recipe} (${backend})`}
                                    onClick={() => handleUninstall(recipe, backend)}>
                                    {isInstalling ? 'Working…' : 'Uninstall'}
                                  </button>
                                )}
                              </div>
                              {showBackendProgress && backendDownload && (
                                <div className="cell__download-progress" aria-label={`${backendProgressPercent(backendDownload).toFixed(0)}%`}>
                                  <div className="cell__download-progress-track">
                                    <div
                                      className="cell__download-progress-fill"
                                      style={{ width: `${backendProgressPercent(backendDownload)}%` }}
                                    />
                                  </div>
                                  <span className="cell__download-progress-text">{backendProgressPercent(backendDownload).toFixed(0)}%</span>
                                </div>
                              )}
                              {backendDownload?.status === 'error' && backendDownload.error && (
                                <span className="cell__download-error">{backendDownload.error}</span>
                              )}
                            </div>
                          );
                        })}
                      </td>
                    );
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {/* #2351: always-present polite live region so NVDA announces toast messages */}
      <div role="status" aria-live="polite" aria-atomic="true" className="sr-only" data-backends-toast-live>
        {toastMsg || ''}
      </div>
      {toastMsg && <div className="backends__toast" data-backends-toast>{toastMsg}</div>}
      </div>
    </section>
  );
};

export default BackendManager;
