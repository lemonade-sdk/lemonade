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
let userModelsWatcher = null;
const userModelsSubscribers = new Set();
const userModelsDestroyedHandlers = new Map();
const SETTINGS_FILE_NAME = 'app_settings.json';
const SETTINGS_UPDATED_CHANNEL = 'settings-updated';
const SERVER_PORT_UPDATED_CHANNEL = 'server-port-updated';
let cachedServerPort = 8000; // Default port
const BASE_APP_SETTING_VALUES = Object.freeze({
  temperature: 0.7,
  topK: 40,
  topP: 0.9,
  repeatPenalty: 1.1,
  enableThinking: true,
});
const DEFAULT_APP_SETTINGS = Object.freeze({
  temperature: { value: BASE_APP_SETTING_VALUES.temperature, useDefault: true },
  topK: { value: BASE_APP_SETTING_VALUES.topK, useDefault: true },
  topP: { value: BASE_APP_SETTING_VALUES.topP, useDefault: true },
  repeatPenalty: { value: BASE_APP_SETTING_VALUES.repeatPenalty, useDefault: true },
  enableThinking: { value: BASE_APP_SETTING_VALUES.enableThinking, useDefault: true },
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

const getUserModelsFilePath = () => {
  const cacheDir = getCacheDirectory();
  if (!cacheDir) {
    return '';
  }
  return path.join(cacheDir, 'user_models.json');
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

const readUserModelsFile = async () => {
  const userModelsPath = getUserModelsFilePath();
  if (!userModelsPath) {
    return {};
  }

  try {
    const content = await fs.promises.readFile(userModelsPath, 'utf-8');
    return JSON.parse(content);
  } catch (error) {
    if (error && error.code === 'ENOENT') {
      return {};
    }
    console.error('Failed to read user_models.json:', error);
    return {};
  }
};

const notifyUserModelsUpdated = () => {
  for (const webContents of userModelsSubscribers) {
    if (!webContents.isDestroyed()) {
      webContents.send('user-models-updated');
    }
  }
};

const disposeUserModelsWatcher = () => {
  if (userModelsSubscribers.size === 0 && userModelsWatcher) {
    userModelsWatcher.close();
    userModelsWatcher = null;
  }
};

const ensureUserModelsWatcher = () => {
  if (userModelsWatcher || userModelsSubscribers.size === 0) {
    return;
  }

  const cacheDir = getCacheDirectory();
  if (!cacheDir) {
    return;
  }

  try {
    fs.mkdirSync(cacheDir, { recursive: true });
  } catch (error) {
    console.warn('Unable to ensure Lemonade cache directory exists:', error);
  }

  try {
    userModelsWatcher = fs.watch(cacheDir, { persistent: false }, (_eventType, filename) => {
      if (!filename) {
        return;
      }

      if (filename.toLowerCase() === 'user_models.json') {
        notifyUserModelsUpdated();
      }
    });
  } catch (error) {
    console.error('Failed to watch user_models.json:', error);
  }
};

const subscribeToUserModels = (webContentsInstance) => {
  if (!webContentsInstance || userModelsSubscribers.has(webContentsInstance)) {
    return;
  }

  userModelsSubscribers.add(webContentsInstance);

  const handleDestroyed = () => {
    userModelsSubscribers.delete(webContentsInstance);
    userModelsDestroyedHandlers.delete(webContentsInstance.id);
    disposeUserModelsWatcher();
    webContentsInstance.removeListener('destroyed', handleDestroyed);
  };

  userModelsDestroyedHandlers.set(webContentsInstance.id, handleDestroyed);
  webContentsInstance.on('destroyed', handleDestroyed);
  ensureUserModelsWatcher();
};

const unsubscribeFromUserModels = (webContentsInstance) => {
  if (!webContentsInstance || !userModelsSubscribers.has(webContentsInstance)) {
    return;
  }

  userModelsSubscribers.delete(webContentsInstance);
  const handler = userModelsDestroyedHandlers.get(webContentsInstance.id);
  if (handler) {
    webContentsInstance.removeListener('destroyed', handler);
    userModelsDestroyedHandlers.delete(webContentsInstance.id);
  }
  disposeUserModelsWatcher();
};

const addUserModelEntry = async (payload = {}) => {
  const { name, checkpoint, recipe, mmproj = '', reasoning = false, vision = false, embedding = false, reranking = false } = payload;

  if (!name || typeof name !== 'string' || !name.trim()) {
    throw new Error('Model name is required');
  }

  if (!checkpoint || typeof checkpoint !== 'string' || !checkpoint.trim()) {
    throw new Error('Checkpoint is required');
  }

  if (!recipe || typeof recipe !== 'string' || !recipe.trim()) {
    throw new Error('Recipe is required');
  }

  const sanitizedName = name.trim();
  const sanitizedCheckpoint = checkpoint.trim();
  const sanitizedRecipe = recipe.trim();
  const sanitizedMmproj = typeof mmproj === 'string' ? mmproj.trim() : '';

  if (sanitizedName.toLowerCase().startsWith('user.')) {
    throw new Error('Do not include the "user." prefix in the model name field');
  }

  if (
    sanitizedCheckpoint.toLowerCase().includes('gguf') &&
    !sanitizedCheckpoint.includes(':')
  ) {
    throw new Error(
      'GGUF checkpoints must include a variant using the CHECKPOINT:VARIANT syntax'
    );
  }

  const userModelsPath = getUserModelsFilePath();
  if (!userModelsPath) {
    throw new Error('Unable to locate the Lemonade cache directory');
  }

  const userModels = await readUserModelsFile();
  if (Object.prototype.hasOwnProperty.call(userModels, sanitizedName)) {
    throw new Error(`Model "${sanitizedName}" already exists`);
  }

  const labels = ['custom'];
  if (reasoning) {
    labels.push('reasoning');
  }
  if (vision) {
    labels.push('vision');
  }
  if (embedding) {
    labels.push('embeddings');
  }
  if (reranking) {
    labels.push('reranking');
  }

  const entry = {
    checkpoint: sanitizedCheckpoint,
    recipe: sanitizedRecipe,
    suggested: true,
    labels,
  };

  if (sanitizedMmproj) {
    entry.mmproj = sanitizedMmproj;
  }

  userModels[sanitizedName] = entry;
  await fs.promises.mkdir(path.dirname(userModelsPath), { recursive: true });
  await fs.promises.writeFile(
    userModelsPath,
    JSON.stringify(userModels, null, 2),
    'utf-8'
  );

  notifyUserModelsUpdated();

  return {
    modelName: sanitizedName,
    entry,
  };
};

ipcMain.handle('read-user-models', async () => {
  return readUserModelsFile();
});

ipcMain.on('start-watch-user-models', (event) => {
  subscribeToUserModels(event.sender);
});

ipcMain.on('stop-watch-user-models', (event) => {
  unsubscribeFromUserModels(event.sender);
});

ipcMain.handle('add-user-model', async (_event, payload) => {
  return addUserModelEntry(payload);
});

ipcMain.handle('get-app-settings', async () => {
  return readAppSettingsFile();
});

ipcMain.handle('save-app-settings', async (_event, payload) => {
  const sanitized = await writeAppSettingsFile(payload);
  broadcastSettingsUpdated(sanitized);
  return sanitized;
});

ipcMain.handle('get-version', async () => {
  try {
    const http = require('http');
    return new Promise((resolve, reject) => {
      const req = http.get(`http://localhost:${cachedServerPort}/api/v1/health`, (res) => {
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
  const port = await discoverServerPort();
  cachedServerPort = port;
  broadcastServerPortUpdated(port);
  return port;
});

ipcMain.handle('get-server-port', async () => {
  return cachedServerPort;
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


