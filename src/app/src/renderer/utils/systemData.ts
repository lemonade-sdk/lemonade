export interface SystemInfo {
  os_version: string;
  physical_memory: string;
  processor: string;
  devices: Devices;
}

interface Devices {
  amd_dgpu: GPUDevice[];
  amd_igpu: GPUDevice;
  cpu: CPUInfo;
  npu: NPUInfo;
  nvidia_dgpu: GPUDevice[];
}

interface Device {
  name: string;
  available: boolean;
  error?: string;
  inference_engines: InferenceEngines;
}

interface GPUDevice extends Device {
  vram_gb: number;
  virtual_mem_gb?: number;
}

interface CPUInfo extends Device {
  cores: number;
  threads: number;
}

interface NPUInfo extends Device {

}

interface InferenceEngines {
  "llamacpp-rocm"?: InferenceEngine,
  "llamacpp-vulkan"?: InferenceEngine,
  oga?: InferenceEngine;
}
interface InferenceEngine {
  available: boolean;
  backend?: string;
  version?: string;
  error?: string;
}

interface SystemData {
  info?: SystemInfo;
}

const fetchSystemInfoFromAPI = async (): Promise<SystemData> => {
  const { serverFetch } = await import('./serverConfig');

  try {
    const response = await serverFetch('/system-info');
    if (!response.ok) {
      throw new Error(`Failed to fetch system info: ${response.status} ${response.statusText}`);
    }

    const data = await response.json();
    const systemInfo: SystemInfo = { ...data };

    return { info: systemInfo };
  } catch (error) {
    console.error('Failed to fetch supported inference data from API:', error);
    return {};
  }
};

export const fetchSystemInfoData = async (): Promise<SystemData> => {
  return fetchSystemInfoFromAPI();
};
