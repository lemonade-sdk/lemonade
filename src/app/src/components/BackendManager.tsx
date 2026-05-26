import React, { useState, useEffect, useCallback, useMemo } from 'react';
import api from '../api';

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
  devices: string[];
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
  'OS Version': string;
  lemonade_version: string;
  devices: {
    cpu: DeviceInfo;
    amd_gpu: DeviceInfo[];
    nvidia_gpu: DeviceInfo[];
    amd_npu?: DeviceInfo;
    metal?: DeviceInfo;
  };
  recipes: Record<string, RecipeInfo>;
}

/* ── Constants ─────────────────────────────────────────────── */

/** User-facing labels for recipes */
const RECIPE_LABELS: Record<string, string> = {
  llamacpp:       'llama.cpp',
  whispercpp:     'whisper.cpp',
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
  'sd-cpp':       'Image',
  kokoro:         'TTS',
  flm:            'LLM',         // NPU LLM — same column
  'ryzenai-llm':  'LLM',         // NPU LLM — same column
  vllm:           'LLM',
};

/** Device display order */
const DEVICE_ORDER = ['cpu', 'amd_gpu', 'nvidia_gpu', 'metal', 'amd_npu'] as const;
const DEVICE_LABELS: Record<string, string> = {
  cpu:         'CPU',
  amd_gpu:     'GPU',
  nvidia_gpu:  'GPU (NVIDIA)',
  metal:       'GPU (Metal)',
  amd_npu:     'NPU',
};

/** Backend → which device row it belongs to */
const BACKEND_DEVICE: Record<string, string> = {
  cpu:            'cpu',
  vulkan:         'amd_gpu',
  rocm:           'amd_gpu',
  'rocm-stable':  'amd_gpu',
  'rocm-nightly': 'amd_gpu',
  metal:          'metal',
  npu:            'amd_npu',
  system:         'cpu',
};

/** Capability columns */
const CAPABILITY_COLS = ['LLM', 'Audio', 'Image', 'TTS'] as const;

/* ── State badge ───────────────────────────────────────────── */

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

/* ── Component ─────────────────────────────────────────────── */

const BackendManager: React.FC = () => {
  const [sysInfo, setSysInfo] = useState<SystemInfoData | null>(null);
  const [loading, setLoading] = useState(true);
  const [showTech, setShowTech] = useState(false);
  const [installing, setInstalling] = useState<string | null>(null); // "recipe:backend"
  const [toastMsg, setToastMsg] = useState<string | null>(null);

  /* ── Fetch system-info ────────────────────────────────── */

  const fetchInfo = useCallback(async () => {
    try {
      setLoading(true);
      const data = await api.systemInfo() as unknown as SystemInfoData;
      setSysInfo(data);
    } catch (err) {
      console.error('Failed to fetch system-info', err);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => { fetchInfo(); }, [fetchInfo]);

  /* ── Actions ──────────────────────────────────────────── */

  const toast = useCallback((msg: string) => {
    setToastMsg(msg);
    setTimeout(() => setToastMsg(null), 3500);
  }, []);

  const handleInstall = useCallback(async (recipe: string, backend: string, isUpdate = false) => {
    const key = `${recipe}:${backend}`;
    const actionLabel = isUpdate ? 'Updating' : 'Installing';
    const doneLabel = isUpdate ? 'updated' : 'installed';
    setInstalling(key);
    toast(`${actionLabel} ${RECIPE_LABELS[recipe] || recipe} · ${backend}…`);
    try {
      await api.installBackend(recipe, backend, {
        onProgress: (d) => {
          if (d.percent != null) {
            setToastMsg(`${actionLabel} ${RECIPE_LABELS[recipe] || recipe} · ${backend}… ${d.percent}%`);
          }
        },
        onComplete: () => {
          toast(`✓ ${RECIPE_LABELS[recipe] || recipe} · ${backend} ${doneLabel}`);
          setInstalling(null);
          fetchInfo();
        },
        onError: (err) => {
          toast(`✗ ${actionLabel} failed: ${err.message}`);
          setInstalling(null);
        },
      });
    } catch (err: any) {
      toast(`✗ ${actionLabel} failed: ${err.message || err}`);
      setInstalling(null);
    }
  }, [fetchInfo, toast]);

  const handleUninstall = useCallback(async (recipe: string, backend: string) => {
    try {
      setInstalling(`${recipe}:${backend}`);
      await api.uninstallBackend(recipe, backend);
      toast(`✓ ${RECIPE_LABELS[recipe] || recipe} · ${backend} uninstalled`);
      fetchInfo();
    } catch (err: any) {
      toast(`✗ Uninstall failed: ${err.message || err}`);
    } finally {
      setInstalling(null);
    }
  }, [fetchInfo, toast]);

  const handleUpdateAll = useCallback(async () => {
    if (!sysInfo?.recipes) return;
    const updates: { recipe: string; backend: string }[] = [];
    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      for (const [backend, bInfo] of Object.entries(recipeInfo.backends)) {
        if (bInfo.state === 'update_required' || bInfo.state === 'update_available') {
          updates.push({ recipe, backend });
        }
      }
    }
    for (const { recipe, backend } of updates) {
      await handleInstall(recipe, backend, true);
    }
  }, [sysInfo, handleInstall]);

  const handleAction = useCallback((url: string) => {
    window.open(url, '_blank', 'noopener,noreferrer');
  }, []);

  /* ── Build the matrix ─────────────────────────────────── */

  // Determine which devices are available
  const availableDevices = useMemo(() => {
    if (!sysInfo) return [] as string[];
    const devs: string[] = [];
    if (sysInfo.devices.cpu?.available) devs.push('cpu');
    if (sysInfo.devices.amd_gpu?.length > 0 && sysInfo.devices.amd_gpu.some(g => g.available)) devs.push('amd_gpu');
    if (sysInfo.devices.nvidia_gpu?.length > 0 && sysInfo.devices.nvidia_gpu.some(g => g.available)) devs.push('nvidia_gpu');
    if (sysInfo.devices.metal?.available) devs.push('metal');
    if (sysInfo.devices.amd_npu?.available) devs.push('amd_npu');
    return devs;
  }, [sysInfo]);

  // Build matrix cells: device × capability → list of {recipe, backend, info}
  type CellEntry = { recipe: string; backend: string; info: BackendInfo };
  const matrixCells = useMemo(() => {
    if (!sysInfo?.recipes) return new Map<string, CellEntry[]>();
    const cells = new Map<string, CellEntry[]>();

    for (const [recipe, recipeInfo] of Object.entries(sysInfo.recipes)) {
      const cap = RECIPE_CAPABILITY[recipe] || 'LLM';
      for (const [backend, backendInfo] of Object.entries(recipeInfo.backends)) {
        const device = BACKEND_DEVICE[backend] || 'cpu';
        const key = `${device}:${cap}`;
        if (!cells.has(key)) cells.set(key, []);
        cells.get(key)!.push({ recipe, backend, info: backendInfo });
      }
    }
    return cells;
  }, [sysInfo]);

  // Check if any updates available
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

  /* ── Device detail row ────────────────────────────────── */

  const deviceDetail = useCallback((deviceKey: string): string => {
    if (!sysInfo) return '';
    switch (deviceKey) {
      case 'cpu': return sysInfo.devices.cpu?.name || '';
      case 'amd_gpu': return sysInfo.devices.amd_gpu?.map(g => g.name).join(', ') || '';
      case 'nvidia_gpu': return sysInfo.devices.nvidia_gpu?.map(g => g.name).join(', ') || '';
      case 'metal': return sysInfo.devices.metal?.name || '';
      case 'amd_npu': return sysInfo.devices.amd_npu?.name || '';
      default: return '';
    }
  }, [sysInfo]);

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
    <section className={`backends${showTech ? ' show-tech' : ''}`} data-view="backends">
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

        {/* Update banner */}
        {updatesAvailable > 0 && (
          <div className="banner banner--warn" data-backends-banner>
            <span className="banner__icon" aria-hidden="true">⚠</span>
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

        {/* Version + system summary */}
        {sysInfo && (
          <div className="backends__summary">
            <span className="backends__version">
              Lemonade {sysInfo.lemonade_version}
            </span>
            <span className="backends__os">{sysInfo['OS Version']}</span>
          </div>
        )}
      </div>

      {/* ── Device × Capability matrix ─────────────────── */}
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
            {DEVICE_ORDER.filter(d => availableDevices.includes(d)).map(deviceKey => (
              <tr key={deviceKey}>
                <th scope="row">
                  {DEVICE_LABELS[deviceKey]}
                  {showTech && (
                    <div className="cell__device-detail">{deviceDetail(deviceKey)}</div>
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
                        const isInstalling = installing === `${recipe}:${backend}`;
                        return (
                          <div className="cell" key={`${recipe}-${backend}`}
                            data-cell={`${recipe}:${backend}`}>
                            <span className="cell__name">
                              {RECIPE_LABELS[recipe] || recipe}
                              {backend !== 'cpu' && backend !== 'npu' && ` · ${backend}`}
                            </span>
                            <span className={`cell__badge ${badge.cls}`}>
                              {badge.label}
                            </span>
                            {showTech && info.version && (
                              <span className="cell__sha">{info.version}</span>
                            )}
                            <div className="cell__actions">
                              {(info.state === 'installable') && (
                                <button
                                  className="cell__swap"
                                  disabled={isInstalling}
                                  onClick={() => handleInstall(recipe, backend)}>
                                  {isInstalling ? 'Installing…' : 'Install'}
                                </button>
                              )}
                              {(info.state === 'update_required' || info.state === 'update_available') && (
                                <button
                                  className="cell__swap"
                                  disabled={isInstalling}
                                  onClick={() => handleInstall(recipe, backend, true)}>
                                  {isInstalling ? 'Updating…' : 'Update'}
                                </button>
                              )}
                              {info.state === 'action_required' && info.action && (
                                <button className="cell__swap"
                                  onClick={() => handleAction(info.action)}>
                                  Setup guide ▸
                                </button>
                              )}
                              {info.state === 'installed' && info.can_uninstall && (
                                <button
                                  className="cell__swap cell__swap--danger"
                                  disabled={isInstalling}
                                  onClick={() => handleUninstall(recipe, backend)}>
                                  Uninstall
                                </button>
                              )}
                            </div>
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

      {/* Toast */}
      {toastMsg && (
        <div className="backends__toast" data-backends-toast>{toastMsg}</div>
      )}
    </section>
  );
};

export default BackendManager;
