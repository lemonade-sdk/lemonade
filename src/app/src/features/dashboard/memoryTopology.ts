function objectValue(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

function arrayValue(value: unknown): Record<string, unknown>[] {
  if (Array.isArray(value)) return value.map(objectValue);
  const single = objectValue(value);
  return Object.keys(single).length > 0 ? [single] : [];
}

function parseGb(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value) && value > 0) return value;
  if (typeof value !== 'string') return null;
  const match = /([\d.]+)/.exec(value);
  if (!match) return null;
  const parsed = Number(match[1]);
  if (!Number.isFinite(parsed) || parsed <= 0) return null;
  return value.toLowerCase().includes('mb') ? parsed / 1024 : parsed;
}

export interface DashboardMemoryTopology {
  unified: boolean;
  hostTotalGb: number | null;
  gpuTotalGb: number | null;
}

/**
 * Detect whether the reported GPU owns a discrete VRAM budget or borrows from
 * system RAM. Lemonade exposes AMD APUs as an integrated GPU with a virtual
 * memory pool; treating that pool as a second budget is misleading because the
 * same physical memory backs both CPU and GPU allocations.
 */
export function dashboardMemoryTopology(systemInfo: Record<string, unknown> | null): DashboardMemoryTopology {
  const info = objectValue(systemInfo);
  const devices = objectValue(info.devices);
  const amdPrimary = arrayValue(devices.amd_gpu);
  const amdDevices = amdPrimary.length > 0
    ? amdPrimary
    : [...arrayValue(devices.amd_igpu), ...arrayValue(devices.amd_dgpu)];
  const nvidiaDevices = arrayValue(devices.nvidia_gpu);
  const metalDevices = arrayValue(devices.metal);
  const available = [...amdDevices, ...nvidiaDevices, ...metalDevices]
    .filter(device => device.available !== false);

  const sharedAmd = amdDevices.find(device => {
    if (device.available === false) return false;
    const vram = Number(device.vram_gb) || 0;
    const virtual = Number(device.virtual_mem_gb) || 0;
    return device.integrated === true || virtual > vram;
  });

  const unified = Boolean(sharedAmd) && available.length === 1;
  const hostTotalGb = parseGb(info['Physical Memory'] ?? info.physical_memory ?? info.memory_gb);
  const discrete = available.find(device => device !== sharedAmd);
  const gpuTotalGb = unified ? null : parseGb(discrete?.vram_gb ?? available[0]?.vram_gb);
  return { unified, hostTotalGb, gpuTotalGb };
}
