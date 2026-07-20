import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { friendlyErrorMessage } from '../api';
import {
  PRESET_STORE_EVENT,
  BackendTuning,
  backendSupportsArgs,
  loadBackendTunings,
  resetBackendTuning,
  saveBackendTuning,
} from '../presetStore';
import { Icon, type IconName } from './Icon';
import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';
import { DownloadListItem, downloadStore, isDownloadActive } from '../features/downloadManager/downloadStore';
import { WorkspacePaneHeader } from './WorkspacePanels';

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
  experimental?: boolean;
  display_name?: string;
}

interface RecipeInfo {
  default_backend: string;
  backends: Record<string, BackendInfo>;
  experimental?: boolean;
  display_name?: string;
  web_display_name?: string;
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
  acestep:         'ACE-Step',
  thinksound:      'ThinkSound',
  openmoss:        'OpenMOSS TTS',
  trellis:         'TRELLIS.2',
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
  acestep:         'Audio',
  thinksound:      'Audio',
  openmoss:        'TTS',
  trellis:         '3D',
};

// Older lemond builds did not expose descriptor.experimental through
// /system-info yet. Keep a compatibility fallback for the recipes that are
// declared experimental in the backend descriptor registry. Newer servers
// remain authoritative through the explicit boolean fields below.
const EXPERIMENTAL_RECIPE_FALLBACK = new Set([
  'acestep',
  'onnxruntime',
  'openmoss',
  'thinksound',
  'trellis',
  'vllm',
]);

function isExperimentalBackend(recipe: string, recipeInfo: RecipeInfo, backendInfo: BackendInfo): boolean {
  const explicit = backendInfo.experimental ?? recipeInfo.experimental;
  if (explicit !== undefined) return explicit;

  const metadata = [
    recipeInfo.display_name,
    recipeInfo.web_display_name,
    backendInfo.display_name,
    backendInfo.message,
  ].filter(Boolean).join(' ').toLowerCase();

  return metadata.includes('experimental') || EXPERIMENTAL_RECIPE_FALLBACK.has(recipe);
}

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
const CAPABILITY_COLS = ['LLM', 'Audio', 'Image', 'TTS', '3D'] as const;

type CapabilityCol = typeof CAPABILITY_COLS[number];
type CellEntry = { recipe: string; backend: string; info: BackendInfo };
type BackendViewFilter = 'all' | 'installed' | 'available' | 'updates' | 'experimental';

const BACKEND_VIEW_FILTERS: Array<[BackendViewFilter, string, string, IconName]> = [
  ['all', 'All backends', 'Complete compatibility matrix', 'layers'],
  ['installed', 'Installed', 'Ready on this machine', 'check'],
  ['available', 'Available', 'Ready to install', 'download'],
  ['updates', 'Updates', 'Newer runtime available', 'rotate-ccw'],
  ['experimental', 'Experimental', 'Preview integrations', 'flask-conical'],
];

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

interface BackendArgsDialogProps {
  backendKeyValue: string | null;
  tuning: BackendTuning | null;
  onSave: (key: string, args: string) => void;
  onClear: (key: string) => void;
  onClose: () => void;
}

const BackendArgsDialog: React.FC<BackendArgsDialogProps> = ({
  backendKeyValue,
  tuning,
  onSave,
  onClear,
  onClose,
}) => {
  const [args, setArgs] = useState('');
  const dialogRef = useRef<HTMLElement>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  useFocusTrap(dialogRef, !!backendKeyValue);

  useEffect(() => {
    setArgs(tuning?.args || '');
  }, [backendKeyValue, tuning?.args]);

  useEffect(() => {
    if (!backendKeyValue) return;
    const frame = window.requestAnimationFrame(() => inputRef.current?.focus());
    return () => window.cancelAnimationFrame(frame);
  }, [backendKeyValue]);

  useEffect(() => {
    if (!backendKeyValue) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose();
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
  }, [backendKeyValue, onClose]);

  if (!backendKeyValue) return null;
  const [recipe, backend] = backendKeyValue.split(':');
  const label = `${RECIPE_LABELS[recipe] || recipe} · ${backend || 'default'}`;
  const hasSavedArgs = Boolean(tuning?.args);

  return (
    <>
      <div className="backend-args-scrim" onClick={onClose} />
      <aside
        ref={dialogRef}
        className="backend-args-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="backend-args-title"
        data-backend-args-dialog={backendKeyValue}
      >
        <div className="backend-args-dialog__head">
          <div>
            <span className="backend-args-dialog__eyebrow">Backend arguments</span>
            <h2 id="backend-args-title">{label}</h2>
          </div>
          <button className="icon-btn" onClick={onClose} aria-label="Close backend arguments">
            <Icon name="x" size={16} aria-hidden="true" />
          </button>
        </div>
        <p className="backend-args-dialog__copy">
          These arguments apply to every model using this exact backend. Model tuning and explicit load options override conflicting values.
        </p>
        {tuning?.source === 'optimized' && (
          <p className="backend-args-dialog__notice" role="status">
            AutoOpt last replaced this backend entry. Saving here changes it to a manual override.
          </p>
        )}
        <label className="field__label" htmlFor="backend-args-value">Arguments</label>
        <textarea
          ref={inputRef}
          id="backend-args-value"
          className="input backend-args-dialog__textarea"
          rows={7}
          value={args}
          onChange={event => setArgs(event.target.value)}
          placeholder="--threads 8 --ctx-size 65536"
          spellCheck={false}
          autoFocus
          data-backend-args-input
        />
        <p className="backend-args-dialog__hint">
          One shell-style argument string. Saving replaces the previous entry for this backend.
        </p>
        <div className="backend-args-dialog__actions">
          {hasSavedArgs && (
            <button className="btn btn--ghost" onClick={() => onClear(backendKeyValue)} data-backend-args-clear>
              Clear
            </button>
          )}
          <span className="backend-args-dialog__spacer" />
          <button className="btn btn--ghost" onClick={onClose}>Cancel</button>
          <button className="btn btn--primary" onClick={() => onSave(backendKeyValue, args)} data-backend-args-save>
            Save backend args
          </button>
        </div>
      </aside>
    </>
  );
};

/* ── Component ─────────────────────────────────────────────── */

interface BackendManagerProps {
  /**
   * The app keeps views mounted and hides inactive views with CSS. Refresh
   * system-info when the Backends view becomes active so status changes made
   * elsewhere are visible without a full page reload.
   */
  isActive?: boolean;
}

const BackendManager: React.FC<BackendManagerProps> = ({ isActive = true }) => {
  const [sysInfo, setSysInfo] = useState<SystemInfoData | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [showTech, setShowTech] = useState(false);
  const [showUnsupported, setShowUnsupported] = useState(false);
  const [viewFilter, setViewFilter] = useState<BackendViewFilter>('all');
  const [railCollapsed, setRailCollapsed] = useState(false);
  const mobileRail = useWorkspaceMobileRail();
  const [installing, setInstalling] = useState<string | null>(null); // "recipe:backend"
  const [toastMsg, setToastMsg] = useState<string | null>(null);
  const [backendTunings, setBackendTunings] = useState<Record<string, BackendTuning>>(loadBackendTunings);
  const [argsEditorKey, setArgsEditorKey] = useState<string | null>(null);
  const [downloadItems, setDownloadItems] = useState<DownloadListItem[]>(() => downloadStore.snapshot());
  const terminalBackendRefreshRef = useRef<Set<string>>(new Set());
  const sysInfoRef = useRef<SystemInfoData | null>(null);
  const argsTriggerRef = useRef<HTMLButtonElement | null>(null);

  useEffect(() => {
    sysInfoRef.current = sysInfo;
  }, [sysInfo]);

  useEffect(() => {
    const reloadTuningState = () => setBackendTunings(loadBackendTunings());
    window.addEventListener(PRESET_STORE_EVENT, reloadTuningState);
    return () => window.removeEventListener(PRESET_STORE_EVENT, reloadTuningState);
  }, []);

  useEffect(() => {
    if (isActive) setBackendTunings(loadBackendTunings());
  }, [isActive]);

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

  useEffect(() => {
    if (!isActive) return;
    void fetchInfo(!sysInfoRef.current);
  }, [fetchInfo, isActive]);

  useEffect(() => api.onModelsChanged(() => {
    if (isActive) void fetchInfo(false);
  }), [fetchInfo, isActive]);

  useEffect(() => downloadStore.subscribe((items) => {
    setDownloadItems(items);
    for (const item of items) {
      if (item.downloadType !== 'backend') continue;
      if (isDownloadActive(item)) continue;
      if (item.status !== 'completed' && item.status !== 'error' && item.status !== 'cancelled') continue;
      const refreshKey = `${item.id}:${item.status}:${item.terminalAt || item.updatedAt}`;
      if (terminalBackendRefreshRef.current.has(refreshKey)) continue;
      terminalBackendRefreshRef.current.add(refreshKey);
      if (isActive) void fetchInfo(false);
    }
  }), [fetchInfo, isActive]);

  /* ── Actions ──────────────────────────────────────────── */

  const toast = useCallback((msg: string) => {
    setToastMsg(msg);
    window.setTimeout(() => setToastMsg(null), 3500);
  }, []);

  const handleInstall = useCallback(async (recipe: string, backend: string, isUpdate = false) => {
    const key = backendKey(recipe, backend);
    const actionLabel = isUpdate ? 'Updating' : 'Installing';
    const doneLabel = isUpdate ? 'updated' : 'installed';
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
    for (const { recipe, backend } of updates) {
      await handleInstall(recipe, backend, true);
    }
  }, [sysInfo, handleInstall]);

  const handleAction = useCallback((url: string) => {
    window.open(url, '_blank', 'noopener,noreferrer');
  }, []);

  const closeArgsEditor = useCallback(() => {
    setArgsEditorKey(null);
    window.requestAnimationFrame(() => argsTriggerRef.current?.focus());
  }, []);

  const handleSaveBackendArgs = useCallback((key: string, args: string) => {
    saveBackendTuning(key, args, 'user');
    setBackendTunings(loadBackendTunings());
    closeArgsEditor();
    toast(args.trim()
      ? `Saved backend arguments for ${key}`
      : `Cleared backend arguments for ${key}`);
  }, [closeArgsEditor, toast]);

  const handleClearBackendArgs = useCallback((key: string) => {
    resetBackendTuning(key);
    setBackendTunings(loadBackendTunings());
    closeArgsEditor();
    toast(`Cleared backend arguments for ${key}`);
  }, [closeArgsEditor, toast]);

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
        const effectiveInfo: BackendInfo = {
          ...backendInfo,
          experimental: isExperimentalBackend(recipe, recipeInfo, backendInfo),
        };
        // Match GUI2: unsupported backends are not useful actions/statuses for
        // the current system and should not take matrix space (#2568).
        if (effectiveInfo.state === 'unsupported') continue;
        for (const device of devicesForBackend(recipe, backend, effectiveInfo)) {
          const key = `${device}:${cap}`;
          if (!cells.has(key)) cells.set(key, []);
          cells.get(key)!.push({ recipe, backend, info: effectiveInfo });
        }
      }
    }
    return cells;
  }, [sysInfo]);

  const unsupportedMatrixCells = useMemo(() => {
    if (!sysInfo?.recipes) return new Map<string, CellEntry[]>();
    const cells = new Map<string, CellEntry[]>();

    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      const cap = (RECIPE_CAPABILITY[recipe] || 'LLM') as CapabilityCol;
      for (const [backend, backendInfo] of Object.entries(recipeInfo.backends)) {
        const effectiveInfo: BackendInfo = {
          ...backendInfo,
          experimental: isExperimentalBackend(recipe, recipeInfo, backendInfo),
        };
        // Keep unsupported backends out of the primary matrix (#2568), but retain
        // a collapsed same-shape matrix for debugging and technical-detail reasons.
        if (effectiveInfo.state !== 'unsupported') continue;
        for (const device of devicesForBackend(recipe, backend, effectiveInfo)) {
          const key = `${device}:${cap}`;
          if (!cells.has(key)) cells.set(key, []);
          cells.get(key)!.push({ recipe, backend, info: effectiveInfo });
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

  const unsupportedMatrixRows = useMemo(() => {
    const referenced = new Set<DeviceKey>();
    for (const key of unsupportedMatrixCells.keys()) {
      const device = key.split(':')[0] as DeviceKey;
      if (DEVICE_ORDER.includes(device)) referenced.add(device);
    }
    return DEVICE_ORDER.filter(d => referenced.has(d));
  }, [unsupportedMatrixCells]);

  const unsupportedBackendCount = useMemo(() => {
    const keys = new Set<string>();
    for (const entries of unsupportedMatrixCells.values()) {
      for (const { recipe, backend } of entries) keys.add(backendKey(recipe, backend));
    }
    return keys.size;
  }, [unsupportedMatrixCells]);

  // Keep the matrix skeleton mounted even when /system-info is unavailable.
  // This preserves navigation/test affordances while the cells truthfully show
  // no backend entries until the server reports real data.
  const matrixRowsForRender = matrixRows.length > 0 ? matrixRows : (['cpu'] as DeviceKey[]);

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

  const backendStateCounts = useMemo(() => {
    const counts = { all: 0, installed: 0, available: 0, updates: 0, experimental: 0 };
    if (!sysInfo?.recipes) return counts;
    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      for (const backendInfo of Object.values(recipeInfo.backends)) {
        if (backendInfo.state === 'unsupported') continue;
        counts.all++;
        if (backendInfo.state === 'installed') counts.installed++;
        if (backendInfo.state === 'installable') counts.available++;
        if (backendInfo.state === 'update_required' || backendInfo.state === 'update_available') counts.updates++;
        if (isExperimentalBackend(recipe, recipeInfo, backendInfo)) counts.experimental++;
      }
    }
    return counts;
  }, [sysInfo]);

  const backendMatchesView = useCallback((entry: CellEntry) => {
    if (viewFilter === 'all') return true;
    if (viewFilter === 'installed') return entry.info.state === 'installed';
    if (viewFilter === 'available') return entry.info.state === 'installable';
    if (viewFilter === 'updates') return entry.info.state === 'update_required' || entry.info.state === 'update_available';
    const recipeInfo = sysInfo?.recipes?.[entry.recipe];
    return recipeInfo ? isExperimentalBackend(entry.recipe, recipeInfo, entry.info) : Boolean(entry.info.experimental);
  }, [sysInfo, viewFilter]);

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

  const renderBackendCell = useCallback(({ recipe, backend, info }: CellEntry) => {
    const badge = stateBadge(info.state);
    const cellKey = backendKey(recipe, backend);
    const isInstalling = installing === cellKey;
    const backendDownload = downloadItems.find(download => backendDownloadMatches(download, recipe, backend));
    const showBackendProgress = Boolean(backendDownload && (isDownloadActive(backendDownload) || backendDownload.status === 'paused'));
    const tuning = backendTunings[cellKey] || null;
    const supportsArgs = backendSupportsArgs(recipe);

    return (
      <div className="cell" key={`${recipe}-${backend}`} data-cell={cellKey}>
        <span className={`cell__name${info.experimental ? ' cell__name--experimental' : ''}`}>
          <span>
            {RECIPE_LABELS[recipe] || recipe}
            {backend !== 'cpu' && backend !== 'npu' && ` · ${backend}`}
          </span>
          {info.experimental && (
            <span className="cell__experimental-icon" role="img" title="experimental" aria-label="experimental">
              <Icon name="flask-conical" size={13} />
            </span>
          )}
        </span>
        <span className={`cell__badge ${badge.cls}`}>{badge.label}</span>
        {showTech && info.version && <span className="cell__sha">{info.version}</span>}
        {tuning && (
          <span
            className={`cell__args-state cell__args-state--${tuning.source}`}
            data-cell-backend-args={tuning.source}
            title={tuning.source === 'optimized'
              ? 'Backend arguments last replaced by AutoOpt'
              : 'Manual backend arguments'}
          >
            <Icon name="terminal-square" size={12} aria-hidden="true" />
            Args · {tuning.source === 'optimized' ? 'AutoOpt' : 'Manual'}
          </span>
        )}
        {showTech && info.message && <span className="cell__message">{info.message}</span>}
        <div className="cell__actions" onClick={e => e.stopPropagation()}>
          {supportsArgs && (
            <button
              type="button"
              className={`cell__args-button${tuning ? ' is-active' : ''}`}
              onClick={event => {
                argsTriggerRef.current = event.currentTarget;
                setArgsEditorKey(cellKey);
              }}
              title={tuning ? 'Edit backend arguments' : 'Add backend arguments'}
              aria-label={`${tuning ? 'Edit' : 'Add'} backend arguments for ${RECIPE_LABELS[recipe] || recipe} (${backend})`}
              data-backend-args-button={cellKey}
            >
              <Icon name="terminal-square" size={14} aria-hidden="true" />
            </button>
          )}
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
  }, [backendTunings, downloadItems, handleAction, handleInstall, handleUninstall, installing, showTech]);


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
    <section className={`backends backends--workspace${railCollapsed ? ' workspace--rail-collapsed' : ''}${showTech ? ' show-tech' : ''}`} data-view="backends">
      {mobileRail.isOpen && <div className="workspace-mobile-rail-backdrop" onClick={mobileRail.close} aria-hidden="true" />}
      <aside
        ref={mobileRail.panelRef}
        id="backend-filters-panel"
        className={`workspace-rail mobile-context-panel backends__rail${railCollapsed && !mobileRail.isOpen ? ' is-collapsed' : ''}${mobileRail.isOpen ? ' is-mobile-open' : ''}`}
        aria-label="Backend filters"
        role={mobileRail.isOpen ? 'dialog' : undefined}
        aria-modal={mobileRail.isOpen ? true : undefined}
      >
        <WorkspaceRailHeader
          title="Filters"
          sidebarLabel="backend filters"
          purpose="filter"
          collapsed={railCollapsed && !mobileRail.isOpen}
          onToggle={() => setRailCollapsed(value => !value)}
          onMobileClose={mobileRail.isOpen ? mobileRail.close : undefined}
        />
        <nav className="workspace-filter-list" aria-label="Backend filters">
          {BACKEND_VIEW_FILTERS.map(([id, label, description, icon]) => (
            <button key={id} type="button" className={`workspace-filter-list__item${viewFilter === id ? ' is-active' : ''}`} aria-current={viewFilter === id ? 'true' : undefined} aria-label={label} title={`${label} — ${description}`} onClick={() => { setViewFilter(id); mobileRail.close(); }}>
              <Icon className="workspace-filter-list__icon" name={icon} size={14} />
              <span className="workspace-filter-list__label">{label}</span>
              <span className="workspace-filter-list__count">{backendStateCounts[id]}</span>
            </button>
          ))}
        </nav>
        <div className="workspace-rail__footer backends__rail-footer">
          <label className="backends__toggle">
            <input
              type="checkbox"
              checked={showTech}
              onChange={e => setShowTech(e.target.checked)}
            />
            <span>Show technical details</span>
          </label>
          {sysInfo && (
            <div className="backends__runtime-meta">
              <strong>Lemonade {lemonadeVersion(sysInfo)}</strong>
              <small>{osVersion(sysInfo)}</small>
            </div>
          )}
        </div>
      </aside>

      <WorkspaceMobileMenuButton
        menuLabel="Open backend filters"
        panelId="backend-filters-panel"
        expanded={mobileRail.isOpen}
        onClick={mobileRail.toggle}
        triggerRef={mobileRail.triggerRef}
      />

      <div className="backends__main workspace-pane">
      <WorkspacePaneHeader
        className="backends__pane-header"
        title="Compatibility matrix"
        subtitle="Runtime availability by device and model capability."
        actions={updatesAvailable > 0 ? (
          <div className="backends__header-update" data-backends-banner>
            <span className="sr-only" data-backends-banner-text>{updatesAvailable} backend update{updatesAvailable > 1 ? 's' : ''} available</span>
            <button className="btn btn--primary" data-backends-banner-action onClick={handleUpdateAll} disabled={installing !== null}>
              {installing ? 'Updating…' : `Update all (${updatesAvailable})`}
            </button>
          </div>
        ) : undefined}
      />

      <div className="backends__head">

        {error && (
          <div className="banner banner--error" data-backends-error>
            <span className="banner__icon" aria-hidden="true"><Icon name="alert" size={16} /></span>
            <span className="banner__text">Could not load backend system info: {error}</span>
            <button className="banner__action" onClick={() => void fetchInfo()} disabled={loading}>Retry</button>
          </div>
        )}

      </div>

      {matrixRows.length === 0 && (
        <p className="sr-only" data-backends-matrix-empty>No backend/device data is available for this Lemonade server yet.</p>
      )}

      {backendStateCounts[viewFilter] === 0 && (
        <div className="backends__filter-empty">
          <Icon name={viewFilter === 'updates' ? 'check' : 'box'} size={24} />
          <strong>No {viewFilter} backends</strong>
          <span>{viewFilter === 'updates' ? 'Every installed backend is current.' : 'No runtimes match this filter on the connected machine.'}</span>
        </div>
      )}

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
              {matrixRowsForRender.map(deviceKey => (
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
                        {entries.filter(backendMatchesView).map(renderBackendCell)}
                      </td>
                    );
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>

        {unsupportedBackendCount > 0 && (
          <section className="backends__unsupported" data-backends-unsupported>
            <button
              type="button"
              className="backends__unsupported-toggle"
              aria-expanded={showUnsupported}
              aria-controls="backends-unsupported-matrix"
              onClick={() => setShowUnsupported(open => !open)}>
              <span className="backends__unsupported-title">
                <Icon name={showUnsupported ? 'chevron-down' : 'chevron-right'} size={14} aria-hidden="true" />
                Unsupported backends
              </span>
              <span className="backends__unsupported-meta">
                {unsupportedBackendCount} hidden from the main table
              </span>
            </button>

            {showUnsupported && (
              <div className="matrix matrix--unsupported" id="backends-unsupported-matrix" data-backends-unsupported-matrix>
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
                    {unsupportedMatrixRows.map(deviceKey => (
                      <tr key={deviceKey}>
                        <th scope="row">
                          {DEVICE_LABELS[deviceKey]}
                          {showTech && (
                            <div className="cell__device-detail">{deviceDetail(deviceKey) || 'reported by backend metadata'}</div>
                          )}
                        </th>
                        {CAPABILITY_COLS.map(cap => {
                          const key = `${deviceKey}:${cap}`;
                          const entries = unsupportedMatrixCells.get(key);
                          if (!entries || entries.length === 0) {
                            return (
                              <td className="matrix__empty" key={cap}>
                                <span aria-hidden="true">—</span>
                              </td>
                            );
                          }
                          return (
                            <td key={cap}>
                              {entries.map(renderBackendCell)}
                            </td>
                          );
                        })}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
          </section>
        )}

      <BackendArgsDialog
        backendKeyValue={argsEditorKey}
        tuning={argsEditorKey ? backendTunings[argsEditorKey] || null : null}
        onSave={handleSaveBackendArgs}
        onClear={handleClearBackendArgs}
        onClose={closeArgsEditor}
      />

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
