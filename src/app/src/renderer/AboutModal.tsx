import React, { useEffect, useRef, useState } from 'react';
import { serverConfig } from './utils/serverConfig';
import { fetchSystemInfoData } from './utils/systemData';

interface AboutModalProps {
  isOpen: boolean;
  onClose: () => void;
}

interface SystemInfo {
  system: string;
  os: string;
  cpu: string;
  gpus: string[];
  gtt_gb?: string;
  vram_gb?: string;
}

type UpdateStatus = 'idle' | 'checking' | 'available' | 'current' | 'error' | 'unsupported';

interface ReleaseAsset {
  name: string;
  browser_download_url: string;
}

interface UpdateInfo {
  latestVersion: string;
  releaseUrl: string;
  downloadUrl: string;
}

const LATEST_RELEASE_URL = 'https://api.github.com/repos/lemonade-sdk/lemonade/releases/latest';

const normalizeVersion = (version: string): string => version.trim().replace(/^v/i, '');

const parseVersion = (version: string) => {
  const cleaned = normalizeVersion(version);
  const [main, preRelease] = cleaned.split('-', 2);
  const parts = main.split('.').map((part) => {
    const parsed = Number.parseInt(part, 10);
    return Number.isFinite(parsed) ? parsed : 0;
  });
  return {
    parts,
    isPrerelease: Boolean(preRelease),
  };
};

const compareVersions = (current: string, latest: string): number => {
  const currentParsed = parseVersion(current);
  const latestParsed = parseVersion(latest);
  const length = Math.max(currentParsed.parts.length, latestParsed.parts.length);
  for (let i = 0; i < length; i += 1) {
    const a = currentParsed.parts[i] ?? 0;
    const b = latestParsed.parts[i] ?? 0;
    if (a !== b) {
      return a > b ? 1 : -1;
    }
  }
  if (currentParsed.isPrerelease !== latestParsed.isPrerelease) {
    return currentParsed.isPrerelease ? -1 : 1;
  }
  return 0;
};

const pickDownloadUrl = (assets: ReleaseAsset[], platform: string, fallback: string): string => {
  if (!assets.length) {
    return fallback;
  }

  const normalizedPlatform = platform.toLowerCase();
  const candidates: string[] = [];

  if (normalizedPlatform.includes('win')) {
    candidates.push('.msi', '.exe', '.zip');
  } else if (normalizedPlatform.includes('darwin') || normalizedPlatform.includes('mac')) {
    candidates.push('.dmg', '.pkg', '.zip');
  } else if (normalizedPlatform.includes('linux')) {
    candidates.push('.appimage', '.deb', '.rpm', '.tar.gz');
  }

  const match = assets.find((asset) =>
    candidates.some((suffix) => asset.name.toLowerCase().endsWith(suffix))
  );
  return match?.browser_download_url ?? fallback;
};

const AboutModal: React.FC<AboutModalProps> = ({ isOpen, onClose }) => {
  const [version, setVersion] = useState<string>('Loading...');
  const [appVersion, setAppVersion] = useState<string>('Loading...');
  const [systemInfo, setSystemInfo] = useState<SystemInfo | null>(null);
  const [isLoadingInfo, setIsLoadingInfo] = useState(false);
  const [updateStatus, setUpdateStatus] = useState<UpdateStatus>('idle');
  const [updateMessage, setUpdateMessage] = useState<string>('Check for updates to see if a newer version is available.');
  const [updateInfo, setUpdateInfo] = useState<UpdateInfo | null>(null);
  const cardRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!isOpen) return;

    setVersion('Loading...');
    setAppVersion('Loading...');
    setIsLoadingInfo(true);

    // Retry logic to handle backend startup delay. /health returns the server
    // version; if it's unreachable for the first few attempts the renderer
    // backs off and shows a friendly fallback.
    const fetchVersionWithRetry = async (retries = 3, delay = 1000) => {
      for (let i = 0; i < retries; i++) {
        try {
          const response = await serverConfig.fetch('/health');
          if (response.ok) {
            const data = await response.json();
            const v = data.version;
            if (v && v !== 'Unknown') {
              setVersion(v);
              return;
            }
          }
        } catch {
          // fall through to retry
        }
        if (i < retries - 1) {
          await new Promise((resolve) => setTimeout(resolve, delay));
        }
      }
      setVersion('Unknown (Backend not running)');
    };

    const fetchSystemInfo = async () => {
      try {
        const { info } = await fetchSystemInfoData();
        if (!info) {
          return;
        }

        const gpus: string[] = [];
        let maxGttGb = 0;
        let maxVramGb = 0;

        const considerAmdGpu = (gpu?: { name?: string; virtual_mem_gb?: number; vram_gb?: number }) => {
          if (!gpu) return;
          if (gpu.name) gpus.push(gpu.name);
          if (typeof gpu.virtual_mem_gb === 'number' && isFinite(gpu.virtual_mem_gb)) {
            maxGttGb = Math.max(maxGttGb, gpu.virtual_mem_gb);
          }
          if (typeof gpu.vram_gb === 'number' && isFinite(gpu.vram_gb)) {
            maxVramGb = Math.max(maxVramGb, gpu.vram_gb);
          }
        };

        considerAmdGpu(info.devices?.amd_igpu);
        info.devices?.amd_dgpu?.forEach(considerAmdGpu);

        info.devices?.nvidia_gpu?.forEach((gpu) => {
          if (gpu?.name) gpus.push(gpu.name);
        });

        const normalized: SystemInfo = {
          system: 'Unknown',
          os: info.os_version || 'Unknown',
          cpu: info.processor || 'Unknown',
          gpus,
          gtt_gb: maxGttGb > 0 ? `${maxGttGb} GB` : 'Unknown',
          vram_gb: maxVramGb > 0 ? `${maxVramGb} GB` : 'Unknown',
        };

        setSystemInfo(normalized);
      } catch (error) {
        console.error('Failed to fetch system info:', error);
      } finally {
        setIsLoadingInfo(false);
      }
    };

    fetchVersionWithRetry();
    fetchSystemInfo();
  }, [isOpen]);

  const runUpdateCheck = async (guard?: { cancelled: boolean }) => {
    const isWebApp = window.api?.isWebApp === true;
    if (isWebApp || !window.api?.getAppVersion) {
      if (!guard?.cancelled) {
        setAppVersion(isWebApp ? 'Web app' : 'Unknown');
        setUpdateStatus('unsupported');
        setUpdateMessage('Update checks are available in the desktop app.');
      }
      return;
    }

    setUpdateStatus('checking');
    setUpdateMessage('Checking for updates...');
    setUpdateInfo(null);

    try {
      const currentVersion = normalizeVersion(await window.api.getAppVersion());
      if (!guard?.cancelled) {
        setAppVersion(currentVersion || 'Unknown');
      }

      const response = await fetch(LATEST_RELEASE_URL, {
        headers: {
          Accept: 'application/vnd.github+json',
        },
      });
      if (!response.ok) {
        throw new Error(`Update check failed with ${response.status}`);
      }
      const data = await response.json();
      const latestVersion = normalizeVersion(String(data.tag_name || data.name || ''));
      if (!latestVersion) {
        throw new Error('No release tag found');
      }
      const releaseUrl = String(data.html_url || 'https://github.com/lemonade-sdk/lemonade/releases/latest');
      const assets = Array.isArray(data.assets) ? (data.assets as ReleaseAsset[]) : [];
      const platform = window.api?.platform || navigator.platform || 'unknown';
      const downloadUrl = pickDownloadUrl(assets, platform, releaseUrl);

      if (guard?.cancelled) {
        return;
      }

      const comparison = compareVersions(currentVersion, latestVersion);
      if (comparison < 0) {
        setUpdateStatus('available');
        setUpdateMessage(`Update available: v${latestVersion}`);
        setUpdateInfo({
          latestVersion,
          releaseUrl,
          downloadUrl,
        });
      } else {
        setUpdateStatus('current');
        setUpdateMessage('You are on the latest version.');
      }
    } catch (error) {
      console.error('Update check failed:', error);
      if (!guard?.cancelled) {
        setUpdateStatus('error');
        setUpdateMessage('Unable to check for updates right now.');
      }
    }
  };

  useEffect(() => {
    if (!isOpen) return;

    const guard = { cancelled: false };
    runUpdateCheck(guard);

    return () => {
      guard.cancelled = true;
    };
  }, [isOpen]);

  useEffect(() => {
    if (!isOpen) return;

    const handleClickOutside = (event: MouseEvent) => {
      if (cardRef.current && !cardRef.current.contains(event.target as Node)) {
        onClose();
      }
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        onClose();
      }
    };

    document.addEventListener('mousedown', handleClickOutside);
    document.addEventListener('keydown', handleKeyDown);

    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [isOpen, onClose]);

  if (!isOpen) return null;

  const handleCheckUpdates = async () => {
    await runUpdateCheck();
  };

  const handleDownloadUpdate = () => {
    const url = updateInfo?.downloadUrl || updateInfo?.releaseUrl;
    if (!url) return;
    if (window.api?.openExternal) {
      window.api.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener');
  };

  return (
    <div className="about-popover" ref={cardRef}>
      <div className="about-popover-header">
        <div>
          <p className="about-popover-title">Lemonade</p>
          <p className="about-popover-subtitle">Local AI control center</p>
        </div>
        <button className="about-popover-close" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="about-popover-body">
        <div className="about-popover-version">
          <span>Server Version</span>
          <span>{version}</span>
        </div>
        <div className="about-popover-version">
          <span>App Version</span>
          <span>{appVersion}</span>
        </div>
        <div className="about-popover-info-row">
          <span className="about-popover-info-label">Update Status</span>
          <span className="about-popover-info-value">{updateMessage}</span>
        </div>
        <div className="about-popover-actions">
          <button className="left-panel-link-btn" onClick={handleCheckUpdates}>
            {updateStatus === 'checking' ? 'Checking...' : 'Check for updates'}
          </button>
          {updateStatus === 'available' && (
            <button className="left-panel-link-btn primary" onClick={handleDownloadUpdate}>
              Download update
            </button>
          )}
        </div>

        {!isLoadingInfo && systemInfo && (
          <>
            {systemInfo.system && systemInfo.system !== 'Unknown' && (
              <div className="about-popover-info-row">
                <span className="about-popover-info-label">System</span>
                <span className="about-popover-info-value">{systemInfo.system}</span>
              </div>
            )}
            {systemInfo.os && systemInfo.os !== 'Unknown' && (
              <div className="about-popover-info-row">
                <span className="about-popover-info-label">OS</span>
                <span className="about-popover-info-value">{systemInfo.os}</span>
              </div>
            )}
            {systemInfo.cpu && systemInfo.cpu !== 'Unknown' && (
              <div className="about-popover-info-row">
                <span className="about-popover-info-label">CPU</span>
                <span className="about-popover-info-value">{systemInfo.cpu}</span>
              </div>
            )}
            {systemInfo.gpus.length > 0 && (
              <div className="about-popover-info-row">
                <span className="about-popover-info-label">GPU{systemInfo.gpus.length > 1 ? 's' : ''}</span>
                <span className="about-popover-info-value">
                  {systemInfo.gpus.join(', ')}
                </span>
              </div>
            )}
            {systemInfo.gtt_gb && systemInfo.gtt_gb !== 'Unknown' && (
              <div className="about-popover-info-row">
                <span className="about-popover-info-label">Shared GPU memory</span>
                <span className="about-popover-info-value">{systemInfo.gtt_gb}</span>
              </div>
            )}
            {systemInfo.vram_gb && systemInfo.vram_gb !== 'Unknown' && (
              <div className="about-popover-info-row">
                <span className="about-popover-info-label">Dedicated GPU memory</span>
                <span className="about-popover-info-value">{systemInfo.vram_gb}</span>
              </div>
            )}
          </>
        )}
      </div>
    </div>
  );
};

export default AboutModal;
