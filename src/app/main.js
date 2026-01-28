const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { exec } = require('child_process');

const DEFAULT_MIN_WIDTH = 400;
const DEFAULT_MIN_HEIGHT = 600;
const ABSOLUTE_MIN_WIDTH = 400;
const MIN_ZOOM_LEVEL = -2;
const MAX_ZOOM_LEVEL = 3;
const ZOOM_STEP = 0.2;

let mainWindow;
let currentMinWidth = DEFAULT_MIN_WIDTH;
const SETTINGS_FILE_NAME = 'app_settings.json';
const SETTINGS_UPDATED_CHANNEL = 'settings-updated';
const SERVER_PORT_UPDATED_CHANNEL = 'server-port-updated';
let cachedServerPort = 8000; // Default port

// Remote server support: explicit base URL from --base-url arg or LEMONADE_APP_BASE_URL env var
// When set, this takes precedence over localhost + port discovery
let configuredServerBaseUrl = null;

/**
 * Parse and normalize a server base URL.
 * - Adds http:// if no protocol specified
 * - Strips trailing slashes
 * - Returns null if URL is invalid
 */
const normalizeServerUrl = (url) => {
  if (!url || typeof url !== 'string') {
    return null;
  }

  let normalized = url.trim();
  if (!normalized) {
    return null;
  }

  // Add http:// if no protocol specified
  if (!normalized.match(/^https?:\/\//i)) {
    normalized = 'http://' + normalized;
  }

  // Strip trailing slashes
  normalized = normalized.replace(/\/+$/, '');

  // Validate URL format
  try {
    new URL(normalized);
    return normalized;
  } catch (e) {
    console.error('Invalid server URL:', url, e.message);
    return null;
  }
};

/**
 * Parse command line arguments for --base-url
 */
const parseBaseUrlArg = () => {
  const args = process.argv.slice(1); // Skip the electron executable
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--base-url' && args[i + 1]) {
      return args[i + 1];
    }
    // Also support --base-url=value format
    if (args[i].startsWith('--base-url=')) {
      return args[i].substring('--base-url='.length);
    }
  }
  return null;
};

/**
 * Initialize server base URL from command line or environment variable.
 * Precedence: --base-url > LEMONADE_APP_BASE_URL > null (fallback to localhost discovery)
 */
const initializeServerBaseUrl = () => {
  // Check command line argument first (highest priority)
  const argUrl = parseBaseUrlArg();
  if (argUrl) {
    const normalized = normalizeServerUrl(argUrl);
    if (normalized) {
      console.log('Using server base URL from --base-url:', normalized);
      configuredServerBaseUrl = normalized;
      return;
    }
  }

  // Check environment variable (second priority)
  const envUrl = process.env.LEMONADE_APP_BASE_URL;
  if (envUrl) {
    const normalized = normalizeServerUrl(envUrl);
    if (normalized) {
      console.log('Using server base URL from LEMONADE_APP_BASE_URL:', normalized);
      configuredServerBaseUrl = normalized;
      return;
    }
  }

  // No explicit URL configured - will use localhost + port discovery
  console.log('No explicit server URL configured, will use localhost with port discovery');
  configuredServerBaseUrl = null;
};

// Initialize on startup
initializeServerBaseUrl();
const BASE_APP_SETTING_VALUES = Object.freeze({
  temperature: 0.7,
  topK: 40,
  topP: 0.9,
  repeatPenalty: 1.1,
  enableThinking: true,
  collapseThinkingByDefault: false,
});
const DEFAULT_APP_SETTINGS = Object.freeze({
  temperature: { value: BASE_APP_SETTING_VALUES.temperature, useDefault: true },
  topK: { value: BASE_APP_SETTING_VALUES.topK, useDefault: true },
  topP: { value: BASE_APP_SETTING_VALUES.topP, useDefault: true },
  repeatPenalty: { value: BASE_APP_SETTING_VALUES.repeatPenalty, useDefault: true },
  enableThinking: { value: BASE_APP_SETTING_VALUES.enableThinking, useDefault: true },
  collapseThinkingByDefault: { value: BASE_APP_SETTING_VALUES.collapseThinkingByDefault, useDefault: true },
});
const NUMERIC_APP_SETTING_LIMITS = Object.freeze({
  temperature: { min: 0, max: 2 },
  topK: { min: 1, max: 100 },
  topP: { min: 0, max: 1 },
  repeatPenalty: { min: 1, max: 2 },
});
const NUMERIC_APP_SETTING_KEYS = ['temperature', 'topK', 'topP', 'repeatPenalty'];

const getHomeDirectory = () => {
  if (typeof os.homedir === 'function') {
    return os.homedir();
  }
  return process.env.HOME || process.env.USERPROFILE || '';
};

const getCacheDirectory = () => {
  const homeDir = getHomeDirectory();
  if (!homeDir) {
    return '';
  }
  return path.join(homeDir, '.cache', 'lemonade');
};

const getAppSettingsFilePath = () => {
  const cacheDir = getCacheDirectory();
  if (!cacheDir) {
    return '';
  }
  return path.join(cacheDir, SETTINGS_FILE_NAME);
};

// Default layout settings - which panels are visible and their sizes
const DEFAULT_LAYOUT_SETTINGS = Object.freeze({
  isChatVisible: true,
  isModelManagerVisible: true,
  isCenterPanelVisible: true,
  isLogsVisible: false,
  modelManagerWidth: 280,
  chatWidth: 350,
  logsHeight: 200,
});

const LAYOUT_SIZE_LIMITS = Object.freeze({
  modelManagerWidth: { min: 200, max: 500 },
  chatWidth: { min: 250, max: 800 },
  logsHeight: { min: 100, max: 400 },
});

const createDefaultAppSettings = () => ({
  temperature: { ...DEFAULT_APP_SETTINGS.temperature },
  topK: { ...DEFAULT_APP_SETTINGS.topK },
  topP: { ...DEFAULT_APP_SETTINGS.topP },
  repeatPenalty: { ...DEFAULT_APP_SETTINGS.repeatPenalty },
  enableThinking: { ...DEFAULT_APP_SETTINGS.enableThinking },
  collapseThinkingByDefault: { ...DEFAULT_APP_SETTINGS.collapseThinkingByDefault },
  layout: { ...DEFAULT_LAYOUT_SETTINGS },
});

const clampValue = (value, min, max) => {
  if (!Number.isFinite(value)) {
    return min;
  }
  return Math.min(Math.max(value, min), max);
};

const sanitizeAppSettings = (incoming = {}) => {
  const sanitized = createDefaultAppSettings();

  NUMERIC_APP_SETTING_KEYS.forEach((key) => {
    const rawSetting = incoming[key];
    if (!rawSetting || typeof rawSetting !== 'object') {
      return;
    }

    const limits = NUMERIC_APP_SETTING_LIMITS[key];
    const useDefault =
      typeof rawSetting.useDefault === 'boolean' ? rawSetting.useDefault : sanitized[key].useDefault;
    const numericValue = useDefault
      ? sanitized[key].value
      : typeof rawSetting.value === 'number'
        ? clampValue(rawSetting.value, limits.min, limits.max)
        : sanitized[key].value;

    sanitized[key] = {
      value: numericValue,
      useDefault,
    };
  });

  const rawEnableThinking = incoming.enableThinking;
  if (rawEnableThinking && typeof rawEnableThinking === 'object') {
    const useDefault =
      typeof rawEnableThinking.useDefault === 'boolean'
        ? rawEnableThinking.useDefault
        : sanitized.enableThinking.useDefault;
    sanitized.enableThinking = {
      value: useDefault
        ? sanitized.enableThinking.value
        : typeof rawEnableThinking.value === 'boolean'
          ? rawEnableThinking.value
          : sanitized.enableThinking.value,
      useDefault,
    };
  }

  const rawCollapseThinkingByDefault = incoming.collapseThinkingByDefault;
  if (rawCollapseThinkingByDefault && typeof rawCollapseThinkingByDefault === 'object') {
    const useDefault =
      typeof rawCollapseThinkingByDefault.useDefault === 'boolean'
        ? rawCollapseThinkingByDefault.useDefault
        : sanitized.collapseThinkingByDefault.useDefault;
    sanitized.collapseThinkingByDefault = {
      value: useDefault
        ? sanitized.collapseThinkingByDefault.value
        : typeof rawCollapseThinkingByDefault.value === 'boolean'
          ? rawCollapseThinkingByDefault.value
          : sanitized.collapseThinkingByDefault.value,
      useDefault,
    };
  }

  // Sanitize layout settings
  const rawLayout = incoming.layout;
  if (rawLayout && typeof rawLayout === 'object') {
    // Sanitize boolean visibility settings
    ['isChatVisible', 'isModelManagerVisible', 'isCenterPanelVisible', 'isLogsVisible'].forEach((key) => {
      if (typeof rawLayout[key] === 'boolean') {
        sanitized.layout[key] = rawLayout[key];
      }
    });

    // Sanitize numeric size settings with limits
    Object.entries(LAYOUT_SIZE_LIMITS).forEach(([key, { min, max }]) => {
      const value = rawLayout[key];
      if (typeof value === 'number' && Number.isFinite(value)) {
        sanitized.layout[key] = Math.min(Math.max(Math.round(value), min), max);
      }
    });
  }

  return sanitized;
};

const readAppSettingsFile = async () => {
  const settingsPath = getAppSettingsFilePath();
  if (!settingsPath) {
    return createDefaultAppSettings();
  }

  try {
    const content = await fs.promises.readFile(settingsPath, 'utf-8');
    return sanitizeAppSettings(JSON.parse(content));
  } catch (error) {
    if (error && error.code === 'ENOENT') {
      return createDefaultAppSettings();
    }
    console.error('Failed to read app settings:', error);
    return createDefaultAppSettings();
  }
};

const writeAppSettingsFile = async (settings) => {
  const settingsPath = getAppSettingsFilePath();
  if (!settingsPath) {
    throw new Error('Unable to locate the Lemonade cache-directory');
  }

  const sanitized = sanitizeAppSettings(settings);

  await fs.promises.mkdir(path.dirname(settingsPath), { recursive: true });
  await fs.promises.writeFile(settingsPath, JSON.stringify(sanitized, null, 2), 'utf-8');

  return sanitized;
};

const broadcastSettingsUpdated = (settings) => {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(SETTINGS_UPDATED_CHANNEL, settings);
  }
};

const broadcastServerPortUpdated = (port) => {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(SERVER_PORT_UPDATED_CHANNEL, port);
  }
};

const discoverServerPort = () => {
  return new Promise((resolve) => {
    exec('lemonade-server status', { timeout: 5000 }, (error, stdout, stderr) => {
      if (error) {
        console.warn('Failed to discover server port:', error);
        resolve(8000); // Fall back to default
        return;
      }

      try {
        // Parse the output to find the port
        // Expected format: "Server is running on port {port}" or "Server is not running"
        const output = stdout.trim();

        // Check if server is not running
        if (output.includes('not running')) {
          console.log('Server is not running, using default port 8000');
          resolve(8000);
          return;
        }

        // Try regex pattern to extract port number
        // Pattern matches: "Server is running on port 8080" or any similar format
        const portMatch = output.match(/port[:\s]+(\d+)/i) ||
                         output.match(/localhost:(\d+)/i) ||
                         output.match(/127\.0\.0\.1:(\d+)/i);

        if (portMatch && portMatch[1]) {
          const port = parseInt(portMatch[1], 10);
          if (!isNaN(port) && port > 0 && port < 65536) {
            console.log('Discovered server port:', port);
            resolve(port);
            return;
          }
        }

        console.warn('Could not parse port from lemonade-server status output:', output);
        resolve(8000);
      } catch (parseError) {
        console.error('Error parsing server status:', parseError);
        resolve(8000);
      }
    });
  });
};

ipcMain.handle('get-app-settings', async () => {
  return readAppSettingsFile();
});

// Returns the configured server base URL, or null if using localhost discovery
ipcMain.handle('get-server-base-url', async () => {
  return configuredServerBaseUrl;
});

ipcMain.handle('save-app-settings', async (_event, payload) => {
  const sanitized = await writeAppSettingsFile(payload);
  broadcastSettingsUpdated(sanitized);
  return sanitized;
});

ipcMain.handle('get-version', async () => {
  try {
    const http = require('http');
    const https = require('https');

    // Use configured base URL or fall back to localhost with cached port
    const baseUrl = configuredServerBaseUrl || `http://localhost:${cachedServerPort}`;
    const healthUrl = `${baseUrl}/api/v1/health`;
    const isHttps = healthUrl.startsWith('https://');
    const httpModule = isHttps ? https : http;

    return new Promise((resolve, reject) => {
      const req = httpModule.get(healthUrl, (res) => {
        let data = '';
        res.on('data', (chunk) => { data += chunk; });
        res.on('end', () => {
          try {
            const parsed = JSON.parse(data);
            resolve(parsed.version || 'Unknown');
          } catch (e) {
            resolve('Unknown');
          }
        });
      });
      req.on('error', () => resolve('Unknown'));
      req.setTimeout(2000, () => {
        req.destroy();
        resolve('Unknown');
      });
    });
  } catch (error) {
    return 'Unknown';
  }
});

ipcMain.handle('discover-server-port', async () => {
  // Skip port discovery if an explicit server URL is configured
  // (port discovery only works for local servers)
  if (configuredServerBaseUrl) {
    console.log('Port discovery skipped - using explicit server URL:', configuredServerBaseUrl);
    return null;
  }

  const port = await discoverServerPort();
  cachedServerPort = port;
  broadcastServerPortUpdated(port);
  return port;
});

ipcMain.handle('get-server-port', async () => {
  return cachedServerPort;
});


// Track CPU usage between calls (for Linux /proc/stat parsing)
let lastCpuStats = null;

// Platform-specific system-wide CPU utilization detection
const getCpuUsage = async () => {
  if (process.platform === 'linux') {
    // Linux: Parse /proc/stat for system-wide CPU usage
    try {
      const content = await fs.promises.readFile('/proc/stat', 'utf-8');
      const firstLine = content.split('\n')[0]; // "cpu  user nice system idle iowait irq softirq steal"
      const parts = firstLine.split(/\s+/).slice(1).map(Number);

      const [user, nice, system, idle, iowait, irq, softirq, steal] = parts;
      const totalIdle = idle + iowait;
      const totalActive = user + nice + system + irq + softirq + steal;
      const total = totalIdle + totalActive;

      if (lastCpuStats) {
        const idleDiff = totalIdle - lastCpuStats.totalIdle;
        const totalDiff = total - lastCpuStats.total;

        lastCpuStats = { totalIdle, total };

        if (totalDiff > 0) {
          return ((totalDiff - idleDiff) / totalDiff) * 100;
        }
      }

      lastCpuStats = { totalIdle, total };
      return 0; // First call, no delta yet
    } catch (err) {
      console.debug('CPU detection (Linux): Failed to read /proc/stat:', err.message);
      return null;
    }

  } else if (process.platform === 'win32') {
    // Windows: Use PowerShell to query CPU utilization
    return new Promise((resolve) => {
      const psCommand = `Get-Counter '\\Processor(_Total)\\% Processor Time' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty CounterSamples | Select-Object -ExpandProperty CookedValue`;
      exec(`powershell -NoProfile -Command "${psCommand}"`, { timeout: 3000 }, (error, stdout) => {
        if (error) {
          console.debug('CPU detection (Windows): PowerShell query failed:', error.message);
          resolve(null);
          return;
        }
        const percent = parseFloat(stdout.trim());
        if (isNaN(percent)) {
          console.debug('CPU detection (Windows): Could not parse CPU percentage from:', stdout.trim());
          resolve(null);
        } else {
          resolve(Math.round(percent * 100) / 100);
        }
      });
    });

  } else if (process.platform === 'darwin') {
    // macOS: Use top to get CPU usage
    return new Promise((resolve) => {
      exec('top -l 1 -n 0 | grep "CPU usage"', { timeout: 3000 }, (error, stdout) => {
        if (error) {
          console.debug('CPU detection (macOS): top query failed:', error.message);
          resolve(null);
          return;
        }
        // Format: "CPU usage: X.X% user, X.X% sys, X.X% idle"
        const match = stdout.match(/(\d+\.?\d*)% user.*?(\d+\.?\d*)% sys/);
        if (match) {
          const userPercent = parseFloat(match[1]);
          const sysPercent = parseFloat(match[2]);
          resolve(Math.round((userPercent + sysPercent) * 100) / 100);
        } else {
          console.debug('CPU detection (macOS): Could not parse CPU usage from:', stdout.trim());
          resolve(null);
        }
      });
    });
  }

  console.debug('CPU detection: Unsupported platform:', process.platform);
  return null;
};

// Platform-specific GPU utilization detection
// On multi-GPU systems, returns the highest utilization (likely the active GPU)
const getGpuUsage = async () => {
  if (process.platform === 'linux') {
    // Linux: Read from sysfs (AMD GPUs expose gpu_busy_percent)
    // Check all GPUs and return the highest utilization
    try {
      const drmPath = '/sys/class/drm';
      const cards = await fs.promises.readdir(drmPath).catch((err) => {
        console.debug('GPU detection: Failed to read /sys/class/drm:', err.message);
        return [];
      });

      let highestUsage = null;
      let highestCard = null;

      for (const card of cards) {
        if (!card.match(/^card\d+$/)) continue;

        const amdBusyPath = path.join(drmPath, card, 'device', 'gpu_busy_percent');
        try {
          const content = await fs.promises.readFile(amdBusyPath, 'utf-8');
          const percent = parseFloat(content.trim());
          if (!isNaN(percent)) {
            if (highestUsage === null || percent > highestUsage) {
              highestUsage = percent;
              highestCard = card;
            }
          }
        } catch (err) {
          console.debug(`GPU detection: ${amdBusyPath} not available:`, err.code || err.message);
        }
      }

      if (highestUsage !== null) {
        console.debug(`GPU detection: Highest usage on ${highestCard}: ${highestUsage}%`);
        return highestUsage;
      }
      console.debug('GPU detection: No GPU with gpu_busy_percent found');
    } catch (error) {
      console.error('Failed to read GPU stats from sysfs:', error);
    }
    return null;

  } else if (process.platform === 'win32') {
    // Windows: Use PowerShell to query GPU utilization
    return new Promise((resolve) => {
      const psCommand = `(Get-Counter '\\GPU Engine(*engtype_3D)\\Utilization Percentage' -ErrorAction SilentlyContinue).CounterSamples | Measure-Object -Property CookedValue -Average | Select-Object -ExpandProperty Average`;
      exec(`powershell -NoProfile -Command "${psCommand}"`, { timeout: 3000 }, (error, stdout) => {
        if (error) {
          console.debug('GPU detection (Windows): PowerShell query failed:', error.message);
          resolve(null);
          return;
        }
        const percent = parseFloat(stdout.trim());
        if (isNaN(percent)) {
          console.debug('GPU detection (Windows): Could not parse GPU percentage from:', stdout.trim());
        }
        resolve(isNaN(percent) ? null : Math.round(percent * 100) / 100);
      });
    });

  } else if (process.platform === 'darwin') {
    // macOS: Use ioreg to query GPU performance stats
    return new Promise((resolve) => {
      exec('ioreg -r -d 1 -c IOAccelerator', { timeout: 2000 }, (error, stdout) => {
        if (error) {
          console.debug('GPU detection (macOS): ioreg query failed:', error.message);
          resolve(null);
          return;
        }
        const match = stdout.match(/"Device Utilization %"\s*=\s*(\d+)/i) ||
                      stdout.match(/"GPU Activity"\s*=\s*(\d+)/i);
        if (!match) {
          console.debug('GPU detection (macOS): No utilization data found in ioreg output');
        }
        resolve(match ? parseFloat(match[1]) : null);
      });
    });
  }

  console.debug('GPU detection: Unsupported platform:', process.platform);
  return null;
};

// Platform-specific VRAM/GTT usage detection (returns used memory in GB)
// For dGPU: returns dedicated VRAM only
// For APU: returns VRAM + GTT combined (since GTT is primary GPU memory)
// On multi-GPU systems, returns memory from the GPU with highest utilization
const getVramUsage = async () => {
  if (process.platform === 'linux') {
    // Linux: Read from AMD sysfs
    try {
      const drmPath = '/sys/class/drm';
      const cards = await fs.promises.readdir(drmPath).catch((err) => {
        console.debug('VRAM detection: Failed to read /sys/class/drm:', err.code || err.message);
        return [];
      });

      let highestUsage = -1;
      let highestCard = null;
      let highestCardMemory = null;

      for (const card of cards) {
        if (!card.match(/^card\d+$/)) continue;

        const devicePath = path.join(drmPath, card, 'device');

        // Read GPU utilization to find the most active GPU
        let gpuUsage = 0;
        try {
          const content = await fs.promises.readFile(path.join(devicePath, 'gpu_busy_percent'), 'utf-8');
          gpuUsage = parseFloat(content.trim()) || 0;
        } catch (err) {
          console.debug(`VRAM detection: ${card} gpu_busy_percent not available:`, err.code || err.message);
        }

        let vramUsed = 0;
        let gttUsed = 0;
        let isDGPU = false;

        // Check for board_info to determine if this is a dGPU
        // board_info is present on discrete GPUs but not APUs
        try {
          await fs.promises.access(path.join(devicePath, 'board_info'));
          isDGPU = true;
          console.debug(`VRAM detection: ${card} has board_info, detected as dGPU`);
        } catch (err) {
          console.debug(`VRAM detection: ${card} has no board_info, detected as APU:`, err.code || err.message);
        }

        // Read VRAM used
        const vramUsedPath = path.join(devicePath, 'mem_info_vram_used');
        try {
          const content = await fs.promises.readFile(vramUsedPath, 'utf-8');
          vramUsed = parseInt(content.trim(), 10) || 0;
        } catch (err) {
          console.debug(`VRAM detection: ${vramUsedPath} not available:`, err.code || err.message);
        }

        // Read GTT used
        const gttUsedPath = path.join(devicePath, 'mem_info_gtt_used');
        try {
          const content = await fs.promises.readFile(gttUsedPath, 'utf-8');
          gttUsed = parseInt(content.trim(), 10) || 0;
        } catch (err) {
          console.debug(`VRAM detection: ${gttUsedPath} not available:`, err.code || err.message);
        }

        // Skip if no memory info found for this card
        if (vramUsed === 0 && gttUsed === 0) {
          console.debug(`VRAM detection: ${card} has no memory info, skipping`);
          continue;
        }

        // Calculate memory for this card
        let cardMemory = 0;
        if (isDGPU) {
          cardMemory = vramUsed;
        } else {
          cardMemory = vramUsed + gttUsed;
        }

        // Track the GPU with highest utilization
        if (gpuUsage > highestUsage || highestCard === null) {
          highestUsage = gpuUsage;
          highestCard = card;
          highestCardMemory = { vramUsed, gttUsed, isDGPU, cardMemory };
        }
      }

      // Return memory from the GPU with highest utilization
      if (highestCard !== null && highestCardMemory !== null) {
        const memGb = highestCardMemory.cardMemory / (1024 * 1024 * 1024);
        if (highestCardMemory.isDGPU) {
          console.debug(`VRAM detection: dGPU ${highestCard} (${highestUsage}% usage), VRAM used: ${memGb.toFixed(2)} GB`);
        } else {
          console.debug(`VRAM detection: APU ${highestCard} (${highestUsage}% usage), VRAM+GTT used: ${memGb.toFixed(2)} GB`);
        }
        return memGb;
      }

      console.debug('VRAM detection: No GPU with memory info found');
    } catch (error) {
      console.error('Failed to read VRAM/GTT stats from sysfs:', error);
    }
    return null;

  } else if (process.platform === 'win32') {
    // Windows: AMD VRAM monitoring not yet implemented
    console.debug('VRAM detection (Windows): Not yet implemented');
    return null;

  } else if (process.platform === 'darwin') {
    // macOS: Metal doesn't expose VRAM usage in a standard way
    console.debug('VRAM detection (macOS): Metal does not expose VRAM usage');
    return null;
  }

  console.debug('VRAM detection: Unsupported platform:', process.platform);
  return null;
};

ipcMain.handle('get-system-stats', async () => {
  try {
    // Get memory info
    const totalMemory = os.totalmem();
    const freeMemory = os.freemem();
    const usedMemory = totalMemory - freeMemory;
    const memoryGb = usedMemory / (1024 * 1024 * 1024);

    // Get system-wide CPU usage (platform-specific)
    const cpuPercent = await getCpuUsage();

    // Get GPU usage (platform-specific)
    const gpuPercent = await getGpuUsage();

    // Get VRAM usage (platform-specific)
    const vramGb = await getVramUsage();

    return {
      cpu_percent: cpuPercent !== null ? Math.min(cpuPercent, 100) : null,
      memory_gb: memoryGb,
      gpu_percent: gpuPercent,
      vram_gb: vramGb,
    };
  } catch (error) {
    console.error('Failed to get system stats:', error);
    return {
      cpu_percent: null,
      memory_gb: 0,
      gpu_percent: null,
      vram_gb: null,
    };
  }
});

function updateWindowMinWidth(requestedWidth) {
  if (!mainWindow || typeof requestedWidth !== 'number' || !isFinite(requestedWidth)) {
    return;
  }

  const safeWidth = Math.max(Math.round(requestedWidth), ABSOLUTE_MIN_WIDTH);

  if (safeWidth === currentMinWidth) {
    return;
  }

  currentMinWidth = safeWidth;
  mainWindow.setMinimumSize(currentMinWidth, DEFAULT_MIN_HEIGHT);
}

const clampZoomLevel = (level) => {
  return Math.min(Math.max(level, MIN_ZOOM_LEVEL), MAX_ZOOM_LEVEL);
};

const adjustZoomLevel = (delta) => {
  if (!mainWindow || mainWindow.isDestroyed()) {
    return;
  }

  const currentLevel = mainWindow.webContents.getZoomLevel();
  const nextLevel = clampZoomLevel(currentLevel + delta);

  if (nextLevel !== currentLevel) {
    mainWindow.webContents.setZoomLevel(nextLevel);
  }
};

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    minWidth: DEFAULT_MIN_WIDTH,
    minHeight: DEFAULT_MIN_HEIGHT,
    backgroundColor: '#000000',
    frame: false,
    icon: path.join(__dirname, '..', '..', 'docs', 'assets', 'favicon.ico'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js')
    }
  });

  // In development, load from dist/renderer; in production from root
  const htmlPath = app.isPackaged
    ? path.join(__dirname, 'dist', 'renderer', 'index.html')
    : path.join(__dirname, 'dist', 'renderer', 'index.html');

  mainWindow.loadFile(htmlPath);

  // Open all external links in the default browser
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    // Open in external browser instead of new Electron window
    shell.openExternal(url);
    return { action: 'deny' }; // Prevent Electron from opening a new window
  });

  // Open DevTools in development mode
  if (!app.isPackaged) {
    mainWindow.webContents.openDevTools();
  }

  // Listen for maximize/unmaximize events
  mainWindow.on('maximize', () => {
    mainWindow.webContents.send('maximize-change', true);
  });

  mainWindow.on('unmaximize', () => {
    mainWindow.webContents.send('maximize-change', false);
  });

  mainWindow.on('closed', function () {
    mainWindow = null;
  });
}


app.on('ready', () => {
  createWindow();

  // Window control handlers
  ipcMain.on('minimize-window', () => {
    if (mainWindow) mainWindow.minimize();
  });

  ipcMain.on('maximize-window', () => {
    if (mainWindow) {
      if (mainWindow.isMaximized()) {
        mainWindow.unmaximize();
      } else {
        mainWindow.maximize();
      }
    }
  });

  ipcMain.on('close-window', () => {
    if (mainWindow) mainWindow.close();
  });

  ipcMain.on('open-external', (event, url) => {
    shell.openExternal(url);
  });

  ipcMain.on('update-min-width', (_event, width) => {
    updateWindowMinWidth(width);
  });

  ipcMain.on('zoom-in', () => {
    adjustZoomLevel(ZOOM_STEP);
  });

  ipcMain.on('zoom-out', () => {
    adjustZoomLevel(-ZOOM_STEP);
  });
});

app.on('window-all-closed', function () {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('activate', function () {
  if (mainWindow === null) {
    createWindow();
  }
});
