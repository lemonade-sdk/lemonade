const { app, BrowserWindow, ipcMain, shell, screen } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { exec, spawn, spawnSync } = require('child_process');
const strict = require('assert/strict');

const DEFAULT_MIN_WIDTH = 400;
const DEFAULT_MIN_HEIGHT = 600;
const ABSOLUTE_MIN_WIDTH = 400;
// Preferred initial window size to properly display center menu with both cards
const PREFERRED_INITIAL_WIDTH = 1440;
const PREFERRED_INITIAL_HEIGHT = 900;
const MIN_ZOOM_LEVEL = -2;
const MAX_ZOOM_LEVEL = 3;
const ZOOM_STEP = 0.2;

const BASE_SETTING_VALUES = {
  temperature: 0.7,
  topK: 40,
  topP: 0.9,
  repeatPenalty: 1.1,
  enableThinking: true,
  collapseThinkingByDefault: false,
  baseURL: '',
  apiKey: '',
};

let mainWindow;
let currentMinWidth = DEFAULT_MIN_WIDTH;
const SETTINGS_FILE_NAME = 'app_settings.json';
const SETTINGS_UPDATED_CHANNEL = 'settings-updated';
const SERVER_PORT_UPDATED_CHANNEL = 'server-port-updated';
const CONNECTION_SETTINGS_UPDATED_CHANNEL = 'connection-settings-updated'
let cachedServerPort = 8000; // Default port

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

const DEFAULT_APP_SETTINGS = Object.freeze({
  temperature: { value: BASE_SETTING_VALUES.temperature, useDefault: true },
  topK: { value: BASE_SETTING_VALUES.topK, useDefault: true },
  topP: { value: BASE_SETTING_VALUES.topP, useDefault: true },
  repeatPenalty: { value: BASE_SETTING_VALUES.repeatPenalty, useDefault: true },
  enableThinking: { value: BASE_SETTING_VALUES.enableThinking, useDefault: true },
  collapseThinkingByDefault: { value: BASE_SETTING_VALUES.collapseThinkingByDefault, useDefault: true },
  baseURL: { value: BASE_SETTING_VALUES.baseURL, useDefault: true },
  apiKey: { value: BASE_SETTING_VALUES.apiKey, useDefault: true },
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
  isMarketplaceVisible: true,  // Renamed from isCenterPanelVisible to reset user preference
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
  baseURL: { ...DEFAULT_APP_SETTINGS.baseURL},
  apiKey: { ...DEFAULT_APP_SETTINGS.apiKey},
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

  const rawBaseURL = incoming.baseURL;
  if (rawBaseURL && typeof rawBaseURL === 'object') {
    const useDefault =
      typeof rawBaseURL.useDefault === 'boolean'
        ? rawBaseURL.useDefault
        : sanitized.baseURL.useDefault;
    sanitized.baseURL = {
      value: useDefault
        ? sanitized.baseURL.value
        : typeof rawBaseURL.value === 'string'
          ? rawBaseURL.value
          : sanitized.baseURL.value,
      useDefault,
    };
  }

  const rawApiKey = incoming.apiKey;
  if (rawApiKey && typeof rawApiKey === 'object') {
    const useDefault =
      typeof rawApiKey.useDefault === 'boolean'
        ? rawApiKey.useDefault
        : sanitized.apiKey.useDefault;
    sanitized.apiKey = {
      value: useDefault
        ? sanitized.apiKey.value
        : typeof rawApiKey.value === 'string'
          ? rawApiKey.value
          : sanitized.apiKey.value,
      useDefault,
    };
  }

  // Sanitize layout settings
  const rawLayout = incoming.layout;
  if (rawLayout && typeof rawLayout === 'object') {
    // Sanitize boolean visibility settings
    ['isChatVisible', 'isModelManagerVisible', 'isMarketplaceVisible', 'isLogsVisible'].forEach((key) => {
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

/**
 * Get base URL from Settings file.
 *
 */
const getBaseURLFromConfig = async () => {
  const settings = await readAppSettingsFile();
  const defaultBaseUrl = settings.baseURL.value;

  if (defaultBaseUrl) {
    const normalized = normalizeServerUrl(defaultBaseUrl);
    if (normalized) {
       return normalized;
    }

    return null;
  }
};

const broadcastSettingsUpdated = (settings) => {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(SETTINGS_UPDATED_CHANNEL, settings);
  }
};

const broadcastConnectionSettingsUpdated = (baseURL, apiKey) => {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(CONNECTION_SETTINGS_UPDATED_CHANNEL, baseURL, apiKey);
  }
};

const broadcastServerPortUpdated = (port) => {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(SERVER_PORT_UPDATED_CHANNEL, port);
  }
};

const fetchWithApiKey = async (entpoint) => {
  let serverUrl = await getBaseURLFromConfig();
  let apiKey = (await readAppSettingsFile()).apiKey.value;

  if (!serverUrl) {
    serverUrl = cachedServerPort ? `http://localhost:${cachedServerPort}` : 'http://localhost:8000';
  }

  const options = {timeout: 3000};

  if(apiKey != null && apiKey != '') {
    options.headers = {
      Authorization: `Bearer ${apiKey}`,
    }
  }

  return await fetch(`${serverUrl}${entpoint}`, options);
}

const ensureTrayRunning = () => {
  return new Promise((resolve) => {
    if (process.platform !== 'darwin') {
      resolve();
      return;
    }

    const binaryPath = '/usr/local/bin/lemonade-server';

    if (!fs.existsSync(binaryPath)) {
      console.error(`CRITICAL: Binary not found at ${binaryPath}`);
      resolve();
      return;
    }

    console.log('--- STARTING TRAY MANUALLY ---');

    // 1. NUCLEAR CLEANUP (Crucial!)
    // We must kill any "Ghost" processes and delete the lock files
    // or the new one will think it's already running and quit immediately.
    try {
      gracefulKillBlocking('lemonade-server tray');

      // Delete the lock file that cause "Already Running" error
      const lock = '/tmp/lemonade_Tray.lock';
      if (fs.existsSync(lock)) fs.unlinkSync(lock);
      console.log('Cleanup complete (Zombies killed, Locks deleted).');
    } catch (e) {
      console.error('Cleanup warning:', e.message);
    }

    // 2. PREPARE ENVIRONMENT
    // macOS GUI apps don't have /usr/local/bin in PATH. We must add it.
    const env = { ...process.env };
    env.PATH = `${env.PATH}:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin`;
    // Ensure it can find libraries in /usr/local/lib
    env.DYLD_LIBRARY_PATH = `${env.DYLD_LIBRARY_PATH}:/usr/local/lib`;

    // 3. LAUNCH
    console.log('Spawning tray process...');
    const trayProcess = spawn(binaryPath, ['tray'], {
      detached: true, // Allows it to run independently of the main window
      env: env,       // Pass our fixed environment
      stdio: 'ignore' // Ignore output so it doesn't hang the parent
    });

    // Unref so Electron doesn't wait for it to exit
    trayProcess.unref();

    console.log(`Tray launched! (PID: ${trayProcess.pid})`);

    // Give it a moment to initialize
    setTimeout(resolve, 1000);
  });
};

function gracefulKillBlocking(processPattern) {
    const TIMEOUT_MS = 30000;

    // Send SIGTERM (Polite kill)
    const killResult = spawnSync('pkill', ['-f', processPattern]);

    // If pkill returned non-zero, the process wasn't running. We are done.
    if (killResult.status !== 0) {
        return;
    }

    // 2. Poll for exit
    const deadline = Date.now() + TIMEOUT_MS;
    let isRunning = true;

    while (isRunning && Date.now() < deadline) {
        // Check if process exists using pgrep
        const checkResult = spawnSync('pgrep', ['-f', processPattern]);

        if (checkResult.status !== 0) {
            // pgrep returned non-zero (process not found) -> It exited!
            isRunning = false;
        } else {
            // Process still exists -> Block for 1 second
            spawnSync('sleep', ['1']);
        }
    }

    // 3. Force Kill (SIGKILL) if the flag is still true
    if (isRunning) {
        spawnSync('pkill', ['-9', '-f', processPattern]);
    }
}

const discoverServerPort = () => {
  //This is the default port to try macos lemonade server on.
  const DEFAULT_PORT = 8000;
  const StatusResponseWaitMs = 5000;

  return new Promise((resolve) => {
    // Always ensure tray is running on macOS, regardless of server status
    ensureTrayRunning().then(() => {
      exec('lemonade-server status', { timeout: StatusResponseWaitMs }, (error, stdout, stderr) => {
        if (error || (stdout && stdout.trim().includes('not running'))) {
          // Server not running, tray should start it
          if (process.platform === 'darwin') {
            console.warn('Server not running, tray should start it. Waiting...');
            setTimeout(() => {
              exec('lemonade-server status', { timeout: StatusResponseWaitMs }, (error3, stdout3, stderr3) => {
                if (error3 || (stdout3 && stdout3.trim().includes('not running'))) {
                  console.warn('Server still not running after waiting, using default port 8000');
                  resolve(DEFAULT_PORT);
                  return;
                }

                // Parse the response
                try {
                  const output = stdout3.trim();
                  const portMatch = output.match(/port[:\s]+(\d+)/i) ||
                                   output.match(/localhost:(\d+)/i) ||
                                   output.match(/127\.0\.0\.1:(\d+)/i);

                  if (portMatch && portMatch[1]) {
                    const port = parseInt(portMatch[1], 10);
                    //Verify parsing since ports can not be less than 0 and not more than 65536 because they are a uint16
                    if (!isNaN(port) && port > 0 && port < 65536) {
                      console.log('Discovered server port after tray start:', port);
                      resolve(port);
                      return;
                    }
                  }

                  console.warn('Could not parse port from status output:', output);
                  resolve(DEFAULT_PORT);
                } catch (parseError) {
                  console.error('Error parsing status:', parseError);
                  resolve(DEFAULT_PORT);
                }
              });
            }, 10000); // Wait 10 seconds for tray to start server
            return;
          } else {
            // On non-macOS platforms, just fall back to default
            console.warn('Failed to discover server port:', error);
            resolve(DEFAULT_PORT);
            return;
          }
        }

        try {
          // Parse the output to find the port
          const output = stdout.trim();

          // Try regex pattern to extract port number
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
          resolve(DEFAULT_PORT);
        } catch (parseError) {
          console.error('Error parsing server status:', parseError);
          resolve(DEFAULT_PORT);
        }
      });
    });
  });
};

// Returns the configured server base URL, or null if using localhost discovery
ipcMain.handle('get-server-base-url', async () => {
  return await getBaseURLFromConfig();
});

ipcMain.handle('get-server-api-key', async () => {
  return (await readAppSettingsFile()).apiKey.value;
});

ipcMain.handle('get-app-settings', async () => {
  return readAppSettingsFile();
});

ipcMain.handle('save-app-settings', async (_event, payload) => {
  const sanitized = await writeAppSettingsFile(payload);
  broadcastSettingsUpdated(sanitized);
  broadcastConnectionSettingsUpdated(sanitized.baseURL.value, sanitized.apiKey.value);
  return sanitized;
});

ipcMain.handle('get-version', async () => {
  try {
    const response = await fetchWithApiKey('/api/v1/health');
    if(response.ok) {
      const data = await response.json();
      return data.version || 'Unknown';
    }
  } catch(error) {
    console.error('Failed to fetch version from server:', error);
    return 'Unknown';
  }
});

ipcMain.handle('discover-server-port', async () => {
  let baseURL = getBaseURLFromConfig();
  if (baseURL) {
    console.log('Port discovery skipped - using explicit server URL:', baseURL);
    ensureTrayRunning();
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

ipcMain.handle('get-system-stats', async () => {
  try {
    const response = await fetchWithApiKey('/api/v1/system-stats');
    if (response.ok) {
      const data = await response.json();
      return {
        cpu_percent: data.cpu_percent,
        memory_gb: data.memory_gb || 0,
        gpu_percent: data.gpu_percent,
        vram_gb: data.vram_gb,
      };
    }
  } catch (error) {
    console.error('Failed to fetch system stats from server:', error);
  }

  // Return null stats if server is unavailable
  return {
    cpu_percent: null,
    memory_gb: 0,
    gpu_percent: null,
    vram_gb: null,
  };
});

ipcMain.handle('get-system-info', async () => {
  try {
    const response = await fetchWithApiKey('/api/v1/system-info');
    if (response.ok) {
      const data = await response.json();
      let maxGttGb = 0;
      let maxVramGb = 0;

      const considerAmdGpu = (gpu) => {
        if (gpu && typeof gpu.virtual_mem_gb === 'number' && isFinite(gpu.virtual_mem_gb)) {
          maxGttGb = Math.max(maxGttGb, gpu.virtual_mem_gb);
        }
        if (gpu && typeof gpu.vram_gb === 'number' && isFinite(gpu.vram_gb)) {
          maxVramGb = Math.max(maxVramGb, gpu.vram_gb);
        }
      };

      if (data.devices?.amd_igpu) {
        considerAmdGpu(data.devices.amd_igpu);
      }
      if (Array.isArray(data.devices?.amd_dgpu)) {
        data.devices.amd_dgpu.forEach(considerAmdGpu);
      }

      // Transform server response to match the About window format
      const systemInfo = {
        system: 'Unknown',
        os: data['OS Version'] || 'Unknown',
        cpu: data['Processor'] || 'Unknown',
        gpus: [],
        gtt_gb: maxGttGb > 0 ? `${maxGttGb} GB` : undefined,
        vram_gb: maxVramGb > 0 ? `${maxVramGb} GB` : undefined,
      };

      // Extract GPU information from devices
      if (data.devices) {
        if (data.devices.amd_igpu?.name) {
          systemInfo.gpus.push(data.devices.amd_igpu.name);
        }
        if (data.devices.nvidia_igpu?.name) {
          systemInfo.gpus.push(data.devices.nvidia_igpu.name);
        }
        if (Array.isArray(data.devices.amd_dgpu)) {
          data.devices.amd_dgpu.forEach(gpu => {
            if (gpu.name) systemInfo.gpus.push(gpu.name);
          });
        }
        if (Array.isArray(data.devices.nvidia_dgpu)) {
          data.devices.nvidia_dgpu.forEach(gpu => {
            if (gpu.name) systemInfo.gpus.push(gpu.name);
          });
        }
      }

      return systemInfo;
    }
  } catch (error) {
    console.error('Failed to fetch system info from server:', error);
  }

  // Return default if server is unavailable
  return {
    system: 'Unknown',
    os: 'Unknown',
    cpu: 'Unknown',
    gpus: [],
    gtt_gb: 'Unknown',
    vram_gb: 'Unknown',
  };
});

// Get local marketplace file URL for development fallback
ipcMain.handle('get-local-marketplace-url', async () => {
  // In development, the marketplace.html is in the docs folder at project root
  // In production, it would be bundled differently
  const docsPath = app.isPackaged
    ? path.join(process.resourcesPath, 'docs', 'marketplace.html')
    : path.join(__dirname, '..', '..', 'docs', 'marketplace.html');

  // Check if file exists
  if (fs.existsSync(docsPath)) {
    return `file://${docsPath}?embedded=true&theme=dark`;
  }
  return null;
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
  // Get the primary display's work area (excludes taskbar/dock)
  const primaryDisplay = screen.getPrimaryDisplay();
  const { width: screenWidth, height: screenHeight } = primaryDisplay.workAreaSize;

  // Use preferred size or 90% of screen size, whichever is smaller
  const initialWidth = Math.min(PREFERRED_INITIAL_WIDTH, Math.floor(screenWidth * 0.9));
  const initialHeight = Math.min(PREFERRED_INITIAL_HEIGHT, Math.floor(screenHeight * 0.9));

  mainWindow = new BrowserWindow({
    width: initialWidth,
    height: initialHeight,
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
  ensureTrayRunning();
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
