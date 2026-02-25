import React, { useEffect, useRef, useState } from 'react';
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

const AboutModal: React.FC<AboutModalProps> = ({ isOpen, onClose }) => {
  const [version, setVersion] = useState<string>('Loading...');
  const [systemInfo, setSystemInfo] = useState<SystemInfo | null>(null);
  const [isLoadingInfo, setIsLoadingInfo] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (isOpen && window.api?.getVersion) {
      setVersion('Loading...');
      setIsLoadingInfo(true);

      // Retry logic to handle backend startup delay
      const fetchVersionWithRetry = async (retries = 3, delay = 1000) => {
        for (let i = 0; i < retries; i++) {
          const v = await window.api.getVersion!();
          if (v !== 'Unknown') {
            setVersion(v);
            return;
          }
          if (i < retries - 1) {
            await new Promise(resolve => setTimeout(resolve, delay));
          }
        }
        setVersion('Unknown (Backend not running)');
      };

      const fetchSystemInfo = async () => {
        try {
          if (window.api?.getSystemInfo) {
            const info = await window.api.getSystemInfo();
            console.log('SystemInfo received in AboutModal:', info);
            setSystemInfo(info);
            return;
          }

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

          info.devices?.nvidia_dgpu?.forEach((gpu) => {
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
    }
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
          <span>Version</span>
          <span>{version}</span>
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
