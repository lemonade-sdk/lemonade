const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { spawn } = require('child_process');

const DEFAULT_MIN_WIDTH = 400;
const DEFAULT_MIN_HEIGHT = 600;
const ABSOLUTE_MIN_WIDTH = 400;
const MIN_ZOOM_LEVEL = -2;
const MAX_ZOOM_LEVEL = 3;
const ZOOM_STEP = 0.2;

let mainWindow;
let backendProcess;
let currentMinWidth = DEFAULT_MIN_WIDTH;
let userModelsWatcher = null;
const userModelsSubscribers = new Set();
const userModelsDestroyedHandlers = new Map();
const SETTINGS_FILE_NAME = 'app_settings.json';
const SETTINGS_UPDATED_CHANNEL = 'settings-updated';
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

const createDefaultAppSettings = () => ({
  temperature: { ...DEFAULT_APP_SETTINGS.temperature },
  topK: { ...DEFAULT_APP_SETTINGS.topK },
  topP: { ...DEFAULT_APP_SETTINGS.topP },
  repeatPenalty: { ...DEFAULT_APP_SETTINGS.repeatPenalty },
  enableThinking: { ...DEFAULT_APP_SETTINGS.enableThinking },
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
  const { name, checkpoint, recipe, mmproj = '', reasoning = false, vision = false } = payload;

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
      const req = http.get('http://localhost:8000/api/v1/health', (res) => {
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

function startTypeScriptBackend() {
  // Determine the server script path and Node executable
  let serverScriptPath;
  let nodeExecutable = 'node';
  
  if (app.isPackaged) {
    // In production, compiled backend is in resources
    serverScriptPath = path.join(process.resourcesPath, 'dist', 'backend', 'server.js');
    console.log('Starting TypeScript backend from:', serverScriptPath);
  } else {
    // In development, use the compiled version in dist
    serverScriptPath = path.join(__dirname, 'dist', 'backend', 'server.js');
    console.log('Starting TypeScript backend from:', serverScriptPath);
  }

  // Start Node process
  backendProcess = spawn(nodeExecutable, [serverScriptPath], {
    stdio: ['ignore', 'pipe', 'pipe']
  });

  backendProcess.stdout.on('data', (data) => {
    console.log(`Backend: ${data.toString()}`);
  });

  backendProcess.stderr.on('data', (data) => {
    console.error(`Backend Error: ${data.toString()}`);
  });

  backendProcess.on('error', (error) => {
    console.error('Failed to start backend process:', error);
  });

  backendProcess.on('close', (code) => {
    console.log(`Backend process exited with code ${code}`);
    if (code !== 0 && code !== null) {
      console.error('Backend exited unexpectedly');
    }
  });
}

function stopBackend() {
  if (backendProcess) {
    backendProcess.kill();
    backendProcess = null;
  }
}

app.on('ready', () => {
  startTypeScriptBackend();
  // Give backend a moment to start
  setTimeout(createWindow, 1000);
  
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
    stopBackend();
    app.quit();
  }
});

app.on('activate', function () {
  if (mainWindow === null) {
    createWindow();
  }
});

app.on('before-quit', () => {
  stopBackend();
});

