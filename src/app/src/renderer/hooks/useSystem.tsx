import React, {createContext, useCallback, useContext, useEffect, useMemo, useState} from "react";
import {fetchSystemInfoData, SystemInfo} from "../utils/systemData";

interface SystemContextValue {
  systemInfo?: SystemInfo;
  isLoading: boolean;
  supportedEngines: InferenceEngine[]
  refresh: () => Promise<void>;
}

type InferenceEngine = 'ROCm' | 'Vulkan' | 'OGA' | 'Metal' | 'CPU'

const SystemContext = createContext<SystemContextValue | null>(null);
const platform = window.api?.platform ?? navigator?.platform ?? '';
const normalizedPlatform = platform.toLowerCase();
const isMacPlatform = normalizedPlatform.includes('darwin') || normalizedPlatform.includes('mac');
const isWindowsPlatform = normalizedPlatform.includes('win');

export const SystemProvider: React.FC<{ children: React.ReactNode }> = ({children}) => {
  const [systemInfo, setSystemInfo] = useState<SystemInfo>();
  const [isLoading, setIsLoading] = useState(true);

  // return supported inference engines
  const supportedEngines = useMemo<InferenceEngine[]>(() => {
    let supportedEngines: Set<InferenceEngine> = new Set();
    if (!systemInfo) return [];

    const {amd_dgpu, amd_igpu, nvidia_dgpu, npu, cpu} = systemInfo.devices;
    const amdDevices = [...amd_dgpu, amd_igpu];

    amdDevices.forEach(device => {
      if (!device.inference_engines) return;
      if (device.inference_engines['llamacpp-rocm']?.available) supportedEngines.add('ROCm');
      if (device.inference_engines['llamacpp-vulkan']?.available) supportedEngines.add('Vulkan');
    });

    nvidia_dgpu.forEach(device => {
      if (!device.inference_engines) return;
      if (device.inference_engines['llamacpp-vulkan']?.available) supportedEngines.add('Vulkan');
    });

    if (npu.available && npu.inference_engines.oga?.available) supportedEngines.add('OGA');

    if (isMacPlatform) supportedEngines.add('Metal');
    if (cpu.available) supportedEngines.add ('CPU');

    return [...supportedEngines];
  }, [systemInfo]);

  // Fetch system info from the server
  const refresh = useCallback(async () => {
    setIsLoading(true);
    try {
      const data = await fetchSystemInfoData();

      setSystemInfo(data.info);

    } catch (error) {
      console.error('Failed to fetch system info:', error);
    } finally {
      setIsLoading(false);
    }
  }, []);

  // Initial load
  useEffect(() => {
    refresh();
  }, [refresh]);

  const value: SystemContextValue = {
    systemInfo,
    supportedEngines,
    isLoading,
    refresh,
  };

  return (
      <SystemContext.Provider value={value}>
        {children}
      </SystemContext.Provider>
  );
};

export const useSystem = (): SystemContextValue => {
  const context = useContext(SystemContext);
  if (!context) {
    throw new Error('useSystem must be used within a SystemProvider');
  }
  return context;
};
