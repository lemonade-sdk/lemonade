const { app, BrowserWindow, ipcMain, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { spawn } = require('child_process');

const DEFAULT_MIN_WIDTH = 400;
const DEFAULT_MIN_HEIGHT = 600;
const ABSOLUTE_MIN_WIDTH = 400;

let mainWindow;
let backendProcess;
let currentMinWidth = DEFAULT_MIN_WIDTH;
let userModelsWatcher = null;
const userModelsSubscribers = new Set();
const userModelsDestroyedHandlers = new Map();

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

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    minWidth: DEFAULT_MIN_WIDTH,
    minHeight: DEFAULT_MIN_HEIGHT,
    backgroundColor: '#000000',
    frame: false,
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

