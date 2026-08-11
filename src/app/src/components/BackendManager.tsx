import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { friendlyErrorMessage } from '../api';
import {
  MODEL_CONFIGURATION_EVENT,
  BackendTuning,
  backendSupportsArgs,
  loadBackendTunings,
  resetBackendTuning,
  saveBackendTuning,
} from '../modelConfiguration';
import { Icon, type IconName } from './Icon';
import { WorkspaceCatalogLayout, WorkspaceCatalogSection } from './WorkspaceCatalogLayout';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { DownloadListItem, downloadStore, isDownloadActive } from '../features/downloadManager/downloadStore';
import { WorkspaceActionButton, WorkspaceActionGroup, WorkspacePaneHeader } from './WorkspacePanels';

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
  onnxruntime:    'ONNX Runtime',
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

const ENGINE_LOGO_BASE = 'https://raw.githubusercontent.com/lemonade-sdk/assets/main/engines/';

/* plate 'dark' matches logos with a baked-in dark background fill; showName
 * accompanies icon-only logos that don't spell out the engine name.
 * stable_diffusion_cpp.png (near-square collage) and ryzen_ai_sw.png
 * (gradient badge) are intentionally unmapped: they read badly at banner
 * height, so those engines get the text plate instead. */
type EngineLogo = { file: string; plate?: 'dark'; showName?: boolean };

const ENGINE_LOGOS: Record<string, EngineLogo> = {
  llamacpp:       { file: 'llama_cpp.png' },
  onnxruntime:    { file: 'onnx_runtime.png' },
  whispercpp:     { file: 'whisper_cpp.png', plate: 'dark' },
  moonshine:      { file: 'moonshine.png', showName: true },
  kokoro:         { file: 'kokoros.png' },
  flm:            { file: 'fastflowlm.png' },
  vllm:           { file: 'vllm.png' },
  acestep:        { file: 'ace_step.png', plate: 'dark' },
  openmoss:       { file: 'openmoss.png' },
  trellis:        { file: 'trellis.png' },
};

/** User-facing labels for backend variants */
const BACKEND_LABELS: Record<string, string> = {
  cpu:      'CPU',
  system:   'System',
  vulkan:   'Vulkan',
  rocm:     'ROCm',
  cuda:     'CUDA',
  metal:    'Metal',
  npu:      'NPU',
  directml: 'DirectML',
  dml:      'DirectML',
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
type BackendCatalogVariant = CellEntry & { devices: DeviceKey[] };
type BackendCatalogEntry = {
  recipe: string;
  variants: BackendCatalogVariant[];
  devices: DeviceKey[];
};
type BackendCatalogSection = { capability: CapabilityCol; entries: BackendCatalogEntry[] };
type BackendViewFilter = 'all' | 'installed' | 'available' | 'updates' | 'experimental';

const CAPABILITY_LABELS: Record<CapabilityCol, string> = {
  LLM: 'Language models',
  Audio: 'Audio',
  Image: 'Image generation',
  TTS: 'Text to speech',
  '3D': '3D generation',
};

const CAPABILITY_DESCRIPTIONS: Record<CapabilityCol, string> = {
  LLM: 'Chat, completion, embedding, and reranking runtimes.',
  Audio: 'Transcription and sound generation runtimes.',
  Image: 'Image generation and editing runtimes.',
  TTS: 'Text-to-speech runtimes.',
  '3D': '3D asset generation runtimes.',
};

const BACKEND_VIEW_FILTERS: Array<[BackendViewFilter, string, string, IconName]> = [
  ['all', 'All Backends', 'Complete compatibility matrix', 'layers'],
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

type PendingBackendAction = {
  isUpdate: boolean;
  initialVersion: string;
};

type BackendSyncResult = {
  state?: BackendInfo['state'];
  version: string;
  settled: boolean;
  hadResponse: boolean;
};

const BACKEND_STATUS_RETRY_DELAYS_MS = [0, 250, 500, 1000, 2000, 4000, 8000] as const;

class BackendDownloadMissingError extends Error {
  constructor() {
    super('Backend download disappeared before reaching a terminal state');
    this.name = 'BackendDownloadMissingError';
  }
}

function backendActionIsReflected(
  info: BackendInfo | undefined,
  action: PendingBackendAction | undefined,
): boolean {
  if (!info) return false;
  if (info.state === 'installed') return true;
  if (info.state !== 'update_available' || !action) return false;

  if (!action.isUpdate) return true;
  const currentVersion = cleanString(info.version);
  return Boolean(currentVersion && currentVersion !== cleanString(action.initialVersion));
}

function waitForBackendDownloadTerminal(
  recipe: string,
  backend: string,
  signal: AbortSignal,
): Promise<DownloadListItem> {
  return new Promise((resolve, reject) => {
    let settled = false;
    let sawDownload = false;
    let unsubscribe: (() => void) | null = null;
    let unsubscribeWhenReady = false;

    const cleanup = () => {
      signal.removeEventListener('abort', onAbort);
      if (unsubscribe) unsubscribe();
      else unsubscribeWhenReady = true;
    };

    const finish = (callback: () => void) => {
      if (settled) return;
      settled = true;
      cleanup();
      callback();
    };

    const onAbort = () => finish(() => {
      const reason = signal.reason;
      reject(reason instanceof Error ? reason : new Error('Backend download wait cancelled'));
    });

    const inspect = (items: DownloadListItem[]) => {
      const item = items.find(download => backendDownloadMatches(download, recipe, backend));
      if (!item) {
        if (sawDownload) {
          finish(() => reject(new BackendDownloadMissingError()));
        }
        return;
      }
      sawDownload = true;
      if (isDownloadActive(item) || item.running === true) return;
      if (item.status !== 'completed'
        && item.status !== 'error'
        && item.status !== 'cancelled'
        && item.status !== 'paused') return;
      finish(() => resolve(item));
    };

    if (signal.aborted) {
      onAbort();
      return;
    }
    signal.addEventListener('abort', onAbort, { once: true });
    unsubscribe = downloadStore.subscribe(inspect);
    if (unsubscribeWhenReady) unsubscribe();
  });
}

function buildBackendCatalog(cells: Map<string, CellEntry[]>): BackendCatalogSection[] {
  const byCapability = new Map<CapabilityCol, Map<string, BackendCatalogEntry>>();

  for (const [key, entries] of cells) {
    const [device, capability] = key.split(':') as [DeviceKey, CapabilityCol];
    const capabilityEntries = byCapability.get(capability) || new Map<string, BackendCatalogEntry>();
    byCapability.set(capability, capabilityEntries);

    for (const entry of entries) {
      const existing = capabilityEntries.get(entry.recipe);
      if (existing) {
        if (!existing.devices.includes(device)) existing.devices.push(device);
        const variant = existing.variants.find(item => item.backend === entry.backend);
        if (variant) {
          if (!variant.devices.includes(device)) variant.devices.push(device);
        } else {
          existing.variants.push({ ...entry, devices: [device] });
        }
      } else {
        capabilityEntries.set(entry.recipe, {
          recipe: entry.recipe,
          variants: [{ ...entry, devices: [device] }],
          devices: [device],
        });
      }
    }
  }

  const stateRank: Record<BackendInfo['state'], number> = {
    update_required: 0,
    update_available: 1,
    installed: 2,
    action_required: 3,
    installable: 4,
    unsupported: 5,
  };

  const experimentalRank = (entry: BackendCatalogEntry): number =>
    entry.variants.every(variant => variant.info.experimental) ? 1 : 0;

  return CAPABILITY_COLS.map(capability => ({
    capability,
    entries: [...(byCapability.get(capability)?.values() || [])].sort((a, b) => (
      experimentalRank(a) - experimentalRank(b)
      || b.variants.length - a.variants.length
      || (RECIPE_LABELS[a.recipe] || a.recipe).localeCompare(RECIPE_LABELS[b.recipe] || b.recipe)
    )).map(entry => ({
      ...entry,
      variants: entry.variants.sort((a, b) => (
        stateRank[a.info.state] - stateRank[b.info.state]
        || a.backend.localeCompare(b.backend)
      )),
    })),
  })).filter(section => section.entries.length > 0);
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

function releaseLink(info: BackendInfo): string {
  const url = (info.release_url || '').trim();
  if (!/^https?:\/\//.test(url)) return '';
  const path = url.replace(/^https?:\/\//, '');
  if (path.includes('//') || url.endsWith('/tag/')) return '';
  return url;
}

function releaseVersion(url: string): string {
  try {
    const pathname = new URL(url).pathname;
    const marker = '/releases/tag/';
    const markerIndex = pathname.indexOf(marker);
    if (markerIndex < 0) return '';
    return decodeURIComponent(
      pathname.slice(markerIndex + marker.length).replace(/\/$/, ''),
    );
  } catch {
    return '';
  }
}

function releaseLinkForVersion(url: string, version: string): string {
  if (!url || !version) return '';
  try {
    const releaseUrl = new URL(url);
    const marker = '/releases/tag/';
    const markerIndex = releaseUrl.pathname.indexOf(marker);
    if (markerIndex < 0) return '';
    releaseUrl.pathname = `${releaseUrl.pathname.slice(0, markerIndex + marker.length)}${encodeURIComponent(version)}`;
    releaseUrl.search = '';
    releaseUrl.hash = '';
    return releaseUrl.toString();
  } catch {
    return '';
  }
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
          <WorkspaceActionButton size="toolbar" appearance="quiet" icon="x" iconOnly onClick={onClose} aria-label="Close backend arguments" />
        </div>
        <p className="backend-args-dialog__copy">
          These arguments apply to every model using this exact backend. Model tuning and explicit load options override conflicting values.
        </p>
        {tuning?.source === 'optimized' && (
          <p className="backend-args-dialog__notice" role="status">
            These backend arguments were set by an optimizer. Saving here converts them to a manual override.
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
        <WorkspaceActionGroup className="backend-args-dialog__actions" label="Backend argument actions">
          {hasSavedArgs && (
            <WorkspaceActionButton appearance="danger" icon="trash" onClick={() => onClear(backendKeyValue)} data-backend-args-clear>
              Clear
            </WorkspaceActionButton>
          )}
          <span className="backend-args-dialog__spacer" />
          <WorkspaceActionButton onClick={onClose}>Cancel</WorkspaceActionButton>
          <WorkspaceActionButton appearance="primary" icon="check" onClick={() => onSave(backendKeyValue, args)} data-backend-args-save>
            Save backend args
          </WorkspaceActionButton>
        </WorkspaceActionGroup>
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
  const [sysInfo, setSysInfo] = useState<SystemInfoData | null>(() =>
    api.systemInfoData as unknown as SystemInfoData | null
  );
  const [loading, setLoading] = useState(() => !api.systemInfoData);
  const [error, setError] = useState<string | null>(null);
  const [showTech, setShowTech] = useState(false);
  const [showUnsupported, setShowUnsupported] = useState(false);
  const [showLogos, setShowLogos] = useState(true);
  const [viewFilter, setViewFilter] = useState<BackendViewFilter>('all');
  const [installing, setInstalling] = useState<string | null>(null); // "recipe:backend"
  const [toastMsg, setToastMsg] = useState<string | null>(null);
  const [backendTunings, setBackendTunings] = useState<Record<string, BackendTuning>>(loadBackendTunings);
  const [argsEditorKey, setArgsEditorKey] = useState<string | null>(null);
  const [downloadItems, setDownloadItems] = useState<DownloadListItem[]>(() => downloadStore.snapshot());
  const terminalBackendRefreshRef = useRef<Set<string>>(new Set());
  const systemInfoRequestRef = useRef(0);
  const pendingBackendActionsRef = useRef<Map<string, PendingBackendAction>>(new Map());
  const backendSyncPromisesRef = useRef<Map<string, Promise<BackendSyncResult>>>(new Map());
  const toastTimerRef = useRef<number | null>(null);
  const sysInfoRef = useRef<SystemInfoData | null>(sysInfo);
  const argsTriggerRef = useRef<HTMLButtonElement | null>(null);

  useEffect(() => {
    sysInfoRef.current = sysInfo;
  }, [sysInfo]);

  useEffect(() => {
    const reloadTuningState = () => setBackendTunings(loadBackendTunings());
    window.addEventListener(MODEL_CONFIGURATION_EVENT, reloadTuningState);
    return () => window.removeEventListener(MODEL_CONFIGURATION_EVENT, reloadTuningState);
  }, []);

  useEffect(() => {
    if (isActive) setBackendTunings(loadBackendTunings());
  }, [isActive]);

  /* ── Fetch system-info ────────────────────────────────── */

  const loadSystemInfo = useCallback(async (): Promise<SystemInfoData> => {
    const requestId = ++systemInfoRequestRef.current;
    let data: SystemInfoData;
    try {
      data = await api.systemInfo() as unknown as SystemInfoData;
    } catch (err) {
      // A superseded request must not replace a newer successful snapshot with
      // an error banner merely because its slower network call failed later.
      if (requestId !== systemInfoRequestRef.current && sysInfoRef.current) {
        return sysInfoRef.current;
      }
      throw err;
    }
    if (requestId === systemInfoRequestRef.current) {
      sysInfoRef.current = data;
      setSysInfo(data);
    }
    return data;
  }, []);

  const fetchInfo = useCallback(async (showSpinner = true) => {
    try {
      if (showSpinner) setLoading(true);
      setError(null);
      if (!api.healthData) await api.health().catch(() => null);
      await loadSystemInfo();
    } catch (err) {
      setError(friendlyErrorMessage(err));
    } finally {
      if (showSpinner) setLoading(false);
    }
  }, [loadSystemInfo]);

  const syncBackendStatus = useCallback((recipe: string, backend: string): Promise<BackendSyncResult> => {
    const key = backendKey(recipe, backend);
    const existing = backendSyncPromisesRef.current.get(key);
    if (existing) return existing;

    const task = (async (): Promise<BackendSyncResult> => {
      const action = pendingBackendActionsRef.current.get(key);
      let state: BackendInfo['state'] | undefined;
      let version = '';
      let hadResponse = false;

      for (const delay of BACKEND_STATUS_RETRY_DELAYS_MS) {
        if (delay > 0) {
          await new Promise(resolve => window.setTimeout(resolve, delay));
        }

        try {
          const freshInfo = await loadSystemInfo();
          hadResponse = true;
          const backendInfo = freshInfo.recipes?.[recipe]?.backends?.[backend];
          state = backendInfo?.state;
          version = cleanString(backendInfo?.version);
          if (backendActionIsReflected(backendInfo, action)) {
            return { state, version, settled: true, hadResponse };
          }
        } catch {
          // The download is already terminal. Keep retrying transient system-info
          // failures so the user does not need to clear the completed row manually.
        }
      }

      return { state, version, settled: false, hadResponse };
    })().finally(() => {
      backendSyncPromisesRef.current.delete(key);
    });

    backendSyncPromisesRef.current.set(key, task);
    return task;
  }, [loadSystemInfo]);

  useEffect(() => {
    if (!isActive || pendingBackendActionsRef.current.size > 0) return;
    void fetchInfo(!sysInfoRef.current);
  }, [fetchInfo, isActive]);

  useEffect(() => api.onModelsChanged(() => {
    if (isActive && pendingBackendActionsRef.current.size === 0) void fetchInfo(false);
  }), [fetchInfo, isActive]);

  useEffect(() => downloadStore.subscribe((items) => {
    setDownloadItems(items);
    for (const item of items) {
      if (item.downloadType !== 'backend') continue;
      if (isDownloadActive(item)) continue;
      if (item.status !== 'completed' && item.status !== 'error' && item.status !== 'cancelled') continue;
      if (!isActive) continue;

      const name = item.modelName || item.id.replace(/^backend:/, '');
      const separator = name.indexOf(':');
      const key = separator > 0 ? backendKey(name.slice(0, separator), name.slice(separator + 1)) : '';
      if (key && pendingBackendActionsRef.current.has(key)) continue;

      const refreshKey = `${item.id}:${item.status}:${item.terminalAt || item.updatedAt}`;
      if (terminalBackendRefreshRef.current.has(refreshKey)) continue;
      terminalBackendRefreshRef.current.add(refreshKey);
      void fetchInfo(false);
    }
  }), [fetchInfo, isActive]);

  /* ── Actions ──────────────────────────────────────────── */

  const toast = useCallback((msg: string) => {
    if (toastTimerRef.current != null) window.clearTimeout(toastTimerRef.current);
    setToastMsg(msg);
    toastTimerRef.current = window.setTimeout(() => {
      toastTimerRef.current = null;
      setToastMsg(null);
    }, 3500);
  }, []);

  useEffect(() => () => {
    if (toastTimerRef.current != null) window.clearTimeout(toastTimerRef.current);
  }, []);

  const handleInstall = useCallback(async (recipe: string, backend: string, isUpdate = false) => {
    const key = backendKey(recipe, backend);
    const engineName = RECIPE_LABELS[recipe] || recipe;
    const actionLabel = isUpdate ? 'Updating' : 'Installing';
    const doneLabel = isUpdate ? 'updated' : 'installed';
    const initialInfo = sysInfoRef.current?.recipes?.[recipe]?.backends?.[backend];
    const downloadName = backendDownloadName(recipe, backend);
    const waitController = new AbortController();
    let actionUrl = '';

    pendingBackendActionsRef.current.set(key, {
      isUpdate,
      initialVersion: cleanString(initialInfo?.version),
    });
    setInstalling(key);
    toast(`${actionLabel} ${engineName} · ${backend}…`);
    downloadStore.markLocal(downloadName, 'downloading', 'backend');
    const terminalDownloadPromise = waitForBackendDownloadTerminal(recipe, backend, waitController.signal);
    // Observe rejection immediately; the original promise is still awaited below,
    // but this prevents an unhandled rejection if the API call itself fails first.
    void terminalDownloadPromise.catch(() => undefined);

    try {
      await api.installBackend(recipe, backend, {
        onProgress: (d) => {
          const rawStatus = typeof d.status === 'string' ? d.status : '';
          const normalizedStatus = rawStatus.toLowerCase();
          const completed = d.complete === true
            || normalizedStatus === 'completed'
            || normalizedStatus === 'complete'
            || normalizedStatus === 'success'
            || normalizedStatus === 'done';
          const percent = typeof d.percent === 'number' ? d.percent : undefined;
          if (typeof d.action === 'string' && d.action.trim()) actionUrl = d.action.trim();

          downloadStore.upsertFromPull(downloadName, {
            ...d,
            id: backendDownloadId(recipe, backend),
            type: 'backend',
            name: downloadName,
            status: completed ? 'completed' : (rawStatus || 'downloading'),
            complete: completed ? true : d.complete,
            running: completed && typeof d.running !== 'boolean' ? false : d.running,
            percent: completed ? 100 : (percent ?? d.percent),
          }, 'backend');

          if (!completed && percent != null) {
            toast(`${actionLabel} ${engineName} · ${backend}… ${percent}%`);
          }
        },
        onComplete: () => {
          void downloadStore.refresh();
        },
        onError: (err) => {
          downloadStore.upsertFromPull(downloadName, {
            id: backendDownloadId(recipe, backend),
            type: 'backend',
            name: downloadName,
            status: 'error',
            running: false,
            error: friendlyErrorMessage(err),
          }, 'backend');
        },
      });

      if (actionUrl) {
        waitController.abort();
        await terminalDownloadPromise.catch(() => undefined);
        downloadStore.remove(backendDownloadId(recipe, backend));
        window.open(actionUrl, '_blank', 'noopener,noreferrer');
        toast(`${engineName} · ${backend} requires manual setup`);
        return;
      }

      const terminalDownload = await terminalDownloadPromise;
      if (terminalDownload.status === 'paused') {
        toast(`${engineName} · ${backend} installation paused`);
        return;
      }
      if (terminalDownload.status === 'cancelled') {
        toast(`${engineName} · ${backend} installation cancelled`);
        return;
      }
      if (terminalDownload.status === 'error') {
        throw new Error(terminalDownload.error || 'Unknown backend install error');
      }

      const synced = await syncBackendStatus(recipe, backend);
      if (synced.settled) {
        toast(`${engineName} · ${backend} ${doneLabel}`);
      } else if (synced.hadResponse) {
        toast(`${engineName} · ${backend} download completed, but Lemonade still reports the previous backend status`);
      } else {
        toast(`${engineName} · ${backend} download completed, but the backend status could not be refreshed`);
      }
    } catch (err) {
      if (err instanceof BackendDownloadMissingError) {
        const synced = await syncBackendStatus(recipe, backend);
        if (synced.settled) {
          toast(`${engineName} · ${backend} ${doneLabel}`);
          return;
        }
      }

      const message = friendlyErrorMessage(err);
      const current = downloadStore.snapshot()
        .find(download => backendDownloadMatches(download, recipe, backend));
      if (current?.status !== 'error' && current?.status !== 'cancelled' && current?.status !== 'paused') {
        downloadStore.upsertFromPull(downloadName, {
          id: backendDownloadId(recipe, backend),
          type: 'backend',
          name: downloadName,
          status: 'error',
          running: false,
          error: message,
        }, 'backend');
      }
      toast(`${actionLabel} failed: ${message}`);
      void fetchInfo(false);
    } finally {
      waitController.abort();
      pendingBackendActionsRef.current.delete(key);
      setInstalling(current => current === key ? null : current);
      void downloadStore.refresh();
    }
  }, [fetchInfo, syncBackendStatus, toast]);

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
        // the current system and are hidden unless explicitly requested (#2568).
        if (!showUnsupported && effectiveInfo.state === 'unsupported') continue;
        for (const device of devicesForBackend(recipe, backend, effectiveInfo)) {
          const key = `${device}:${cap}`;
          if (!cells.has(key)) cells.set(key, []);
          cells.get(key)!.push({ recipe, backend, info: effectiveInfo });
        }
      }
    }
    return cells;
  }, [showUnsupported, sysInfo]);

  const backendCatalog = useMemo(() => buildBackendCatalog(matrixCells), [matrixCells]);

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
        if (backendInfo.state === 'unsupported' && !showUnsupported) continue;
        counts.all++;
        if (
          backendInfo.state === 'installed'
          || backendInfo.state === 'update_required'
          || backendInfo.state === 'update_available'
        ) counts.installed++;
        if (backendInfo.state === 'installable') counts.available++;
        if (backendInfo.state === 'update_required' || backendInfo.state === 'update_available') counts.updates++;
        if (isExperimentalBackend(recipe, recipeInfo, backendInfo)) counts.experimental++;
      }
    }
    return counts;
  }, [showUnsupported, sysInfo]);

  const backendMatchesView = useCallback((entry: CellEntry) => {
    if (viewFilter === 'all') return true;
    if (viewFilter === 'installed') {
      return entry.info.state === 'installed'
        || entry.info.state === 'update_required'
        || entry.info.state === 'update_available';
    }
    if (viewFilter === 'available') return entry.info.state === 'installable';
    if (viewFilter === 'updates') return entry.info.state === 'update_required' || entry.info.state === 'update_available';
    const recipeInfo = sysInfo?.recipes?.[entry.recipe];
    return recipeInfo ? isExperimentalBackend(entry.recipe, recipeInfo, entry.info) : Boolean(entry.info.experimental);
  }, [sysInfo, viewFilter]);

  const renderBackendCard = useCallback(({ recipe, variants }: BackendCatalogEntry) => {
    const engineName = RECIPE_LABELS[recipe] || recipe;
    const supportsArgs = backendSupportsArgs(recipe);
    const logo = ENGINE_LOGOS[recipe];

    return (
      <article className="workspace-card backend-card" key={recipe} data-recipe={recipe}>
        <div className="workspace-card__head">
          {showLogos ? (
            <>
              <h3 className="sr-only">{engineName}</h3>
              <div
                className={`backend-card__logo${logo?.plate === 'dark' ? ' backend-card__logo--dark' : ''}${logo && !logo.showName ? ' backend-card__logo--image-only' : ''}`}
                aria-hidden="true"
                data-backend-logo
              >
                {logo && (
                  <img
                    src={`${ENGINE_LOGO_BASE}${logo.file}`}
                    alt=""
                    loading="lazy"
                    onError={event => {
                      event.currentTarget.style.display = 'none';
                      event.currentTarget.parentElement!.className = 'backend-card__logo';
                    }}
                  />
                )}
                <span className="backend-card__logo-name">{engineName}</span>
              </div>
            </>
          ) : (
            <h3 className="workspace-card__name backend-card__name">{engineName}</h3>
          )}
        </div>

        <div className="backend-card__variants">
          {variants.map(({ backend, info }) => {
            const badge = stateBadge(info.state);
            const cellKey = backendKey(recipe, backend);
            const isInstalling = installing === cellKey;
            const backendDownload = downloadItems.find(download => backendDownloadMatches(download, recipe, backend));
            const showBackendProgress = Boolean(backendDownload && (isDownloadActive(backendDownload) || backendDownload.status === 'paused'));
            const tuning = backendTunings[cellKey] || null;
            const name = `${engineName} · ${backend}`;
            const version = cleanString(info.version);
            const isUpdate =
              info.state === 'update_required' || info.state === 'update_available';
            const targetReleaseUrl = releaseLink(info);
            const targetVersion = isUpdate ? releaseVersion(targetReleaseUrl) : '';
            const installedReleaseUrl = isUpdate
              ? releaseLinkForVersion(targetReleaseUrl, version)
              : targetReleaseUrl;
            const variantLabel = BACKEND_LABELS[backend] || backend;

            return (
              <div
                className={`backend-card__variant${info.state === 'unsupported' ? ' backend-card__variant--unsupported' : ''}`}
                key={cellKey}
                data-cell={cellKey}
              >
                <div className="backend-card__variant-head">
                  <h4 className="backend-card__variant-name">
                    {variantLabel}
                    {info.experimental && (
                      <span className="cell__experimental-icon" role="img" aria-label="experimental" title="experimental">
                        <Icon name="flask-conical" size={13} aria-hidden="true" />
                      </span>
                    )}
                  </h4>
                  {info.state !== 'installable' && <span className={`cell__badge ${badge.cls}`}>{badge.label}</span>}
                </div>

                <div className="backend-card__variant-meta">
                  {version && (installedReleaseUrl ? (
                    <a
                      className="backend-card__version"
                      href={installedReleaseUrl}
                      target="_blank"
                      rel="noopener noreferrer"
                      title={`Open installed ${engineName} ${version} release page`}
                    >
                      {version}
                      <Icon name="external-link" size={11} aria-hidden="true" />
                    </a>
                  ) : (
                    <span className="backend-card__version">{version}</span>
                  ))}
                  {isUpdate && targetReleaseUrl && targetVersion && targetVersion !== version && (
                    <a
                      className="backend-card__version"
                      href={targetReleaseUrl}
                      target="_blank"
                      rel="noopener noreferrer"
                      title={`Open ${engineName} ${targetVersion} update release page`}
                    >
                      <Icon name="chevron-right" size={11} aria-hidden="true" />
                      {targetVersion}
                      <Icon name="external-link" size={11} aria-hidden="true" />
                    </a>
                  )}
                  {tuning && (
                    <span className={`cell__args-state cell__args-state--${tuning.source}`} data-cell-backend-args={tuning.source}>
                      <Icon name="terminal-square" size={12} aria-hidden="true" />
                      Args · {tuning.source === 'optimized' ? 'Optimized' : 'Manual'}
                    </span>
                  )}
                </div>

                <div className="backend-card__footer">
                  <div className="backend-card__actions">
                    {info.state === 'installable' && (
                      <WorkspaceActionButton
                        size="small"
                        appearance="secondary"
                        icon="download"
                        className="cell__swap"
                        aria-label={`Install ${name}`}
                        disabled={isInstalling}
                        onClick={() => handleInstall(recipe, backend)}
                      >
                        {isInstalling ? 'Installing…' : 'Install'}
                      </WorkspaceActionButton>
                    )}
                    {(info.state === 'update_required' || info.state === 'update_available') && (
                      <WorkspaceActionButton
                        size="small"
                        appearance="primary"
                        icon="rotate-ccw"
                        className="cell__swap"
                        aria-label={`Update ${name}`}
                        disabled={isInstalling}
                        onClick={() => handleInstall(recipe, backend, true)}
                      >
                        {isInstalling ? 'Updating…' : 'Update'}
                      </WorkspaceActionButton>
                    )}
                    {info.state === 'action_required' && info.action && (
                      <WorkspaceActionButton
                        size="small"
                        appearance="secondary"
                        icon="book-open"
                        className="cell__swap"
                        aria-label={`Open setup guide for ${name}`}
                        onClick={() => handleAction(info.action)}
                      >
                        Setup guide
                      </WorkspaceActionButton>
                    )}
                    {canShowUninstall(info) && (
                      <WorkspaceActionButton
                        size="small"
                        appearance="danger"
                        icon="trash"
                        className="cell__swap cell__swap--danger"
                        aria-label={`Uninstall ${name}`}
                        disabled={isInstalling}
                        onClick={() => handleUninstall(recipe, backend)}
                      >
                        {isInstalling ? 'Working…' : 'Uninstall'}
                      </WorkspaceActionButton>
                    )}
                    {supportsArgs && info.state !== 'unsupported' && (
                      <WorkspaceActionButton
                        type="button"
                        size="toolbar"
                        appearance="quiet"
                        icon="terminal-square"
                        iconOnly
                        className={`cell__args-button${tuning ? ' is-active' : ''}`}
                        data-backend-args-button={cellKey}
                        onClick={event => {
                          argsTriggerRef.current = event.currentTarget;
                          setArgsEditorKey(cellKey);
                        }}
                        title={tuning ? 'Edit backend arguments' : 'Add backend arguments'}
                        aria-label={`${tuning ? 'Edit' : 'Add'} backend arguments for ${engineName} (${backend})`}
                      />
                    )}
                  </div>
                </div>

                {showBackendProgress && backendDownload && (
                  <div className="cell__download-progress" aria-label={`${backendProgressPercent(backendDownload).toFixed(0)}%`}>
                    <div className="cell__download-progress-track">
                      <div className="cell__download-progress-fill" style={{ width: `${backendProgressPercent(backendDownload)}%` }} />
                    </div>
                    <span className="cell__download-progress-text">{backendProgressPercent(backendDownload).toFixed(0)}%</span>
                  </div>
                )}
                {backendDownload?.status === 'error' && backendDownload.error ? (
                  <p className="backend-card__note backend-card__note--error">{backendDownload.error}</p>
                ) : ((showTech || info.state === 'update_available' || info.state === 'update_required') && info.message && (
                  <p className="backend-card__note">{info.message}</p>
                ))}
              </div>
            );
          })}
        </div>
      </article>
    );
  }, [backendTunings, downloadItems, handleAction, handleInstall, handleUninstall, installing, showLogos, showTech]);


  /* ── Render ───────────────────────────────────────────── */

  if (loading && !sysInfo) {
    return (
      <section className="backends" data-view="backends">
        <WorkspacePaneHeader className="backends__pane-header" headingLevel={1} title="Inference Backends" />
        <div style={{ display: 'flex', justifyContent: 'center', padding: 'var(--space-8)' }}>
          <div className="hf-zone__spinner" />
        </div>
      </section>
    );
  }

  return (
    <WorkspaceCatalogLayout
      view="backends"
      className={`backends backends--workspace${showTech ? ' show-tech' : ''}`}
      panelId="backend-filters-panel"
      railTitle="Filters"
      railLabel="Backend filters"
      sidebarLabel="backend filters"
      mobileMenuLabel="Open backend filters"
      filters={BACKEND_VIEW_FILTERS.map(([id, label, description, icon]) => ({
        id,
        label,
        description,
        icon,
        count: backendStateCounts[id],
      }))}
      activeFilter={viewFilter}
      onFilterChange={setViewFilter}
      railFooter={
        <div className="backends__rail-footer">
          <label className="backends__toggle">
            <input
              type="checkbox"
              checked={showTech}
              onChange={e => setShowTech(e.target.checked)}
            />
            <span>Show technical details</span>
          </label>
          <label className="backends__toggle">
            <input
              type="checkbox"
              checked={showUnsupported}
              onChange={e => setShowUnsupported(e.target.checked)}
              data-backends-unsupported-toggle
            />
            <span>Show unsupported backends</span>
          </label>
          <label className="backends__toggle">
            <input
              type="checkbox"
              checked={showLogos}
              onChange={e => setShowLogos(e.target.checked)}
              data-backends-logo-toggle
            />
            <span>Show logos</span>
          </label>
          {sysInfo && (
            <div className="backends__runtime-meta">
              <strong>Lemonade {lemonadeVersion(sysInfo)}</strong>
              <small>{osVersion(sysInfo)}</small>
            </div>
          )}
        </div>
      }
      header={
        <WorkspacePaneHeader
          className="backends__pane-header"
          headingLevel={1}
          title="Inference Backends"
          subtitle="Install and update the inference engines available on this machine."
          actions={updatesAvailable > 0 ? (
            <div className="backends__header-update" data-backends-banner>
              <span className="sr-only" data-backends-banner-text>{updatesAvailable} backend update{updatesAvailable > 1 ? 's' : ''} available</span>
              <WorkspaceActionButton appearance="primary" icon="rotate-ccw" data-backends-banner-action onClick={handleUpdateAll} disabled={installing !== null}>
                {installing ? 'Updating…' : `Update all (${updatesAvailable})`}
              </WorkspaceActionButton>
            </div>
          ) : undefined}
        />
      }
      preContent={<>
        <div className="backends__head">
          {error && (
            <div className="banner banner--error" data-backends-error>
              <span className="banner__icon" aria-hidden="true"><Icon name="alert" size={16} /></span>
              <span className="banner__text">Could not load backend system info: {error}</span>
              <WorkspaceActionButton size="small" icon="rotate-ccw" onClick={() => void fetchInfo()} disabled={loading}>Retry</WorkspaceActionButton>
            </div>
          )}
        </div>
        {backendCatalog.length === 0 && (
          <p className="sr-only" data-backends-matrix-empty>No backend data is available for this Lemonade server yet.</p>
        )}
        {backendStateCounts[viewFilter] === 0 && (
          <div className="backends__filter-empty">
            <Icon name={viewFilter === 'updates' ? 'check' : 'box'} size={24} />
            <strong>No {viewFilter} backends</strong>
            <span>{viewFilter === 'updates' ? 'Every installed backend is current.' : 'No runtimes match this filter on the connected machine.'}</span>
          </div>
        )}
      </>}
      overlay={<>
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
      </>}
    >
      <div className="workspace-catalog" data-backends-matrix>
        {backendCatalog.map(({ capability, entries }) => {
          const visibleEntries = entries
            .map(entry => {
              const variants = entry.variants.filter(backendMatchesView);
              return {
                ...entry,
                variants,
                devices: uniq(variants.flatMap(variant => variant.devices)),
              };
            })
            .filter(entry => entry.variants.length > 0);
          if (visibleEntries.length === 0) return null;
          return (
            <WorkspaceCatalogSection
              key={capability}
              title={CAPABILITY_LABELS[capability]}
              description={CAPABILITY_DESCRIPTIONS[capability]}
            >
              {visibleEntries.map(renderBackendCard)}
            </WorkspaceCatalogSection>
          );
        })}
      </div>
    </WorkspaceCatalogLayout>
  );
};

export default BackendManager;
