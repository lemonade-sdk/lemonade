const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  // Expose any APIs you need here
  platform: process.platform,
  minimizeWindow: () => ipcRenderer.send('minimize-window'),
  maximizeWindow: () => ipcRenderer.send('maximize-window'),
  closeWindow: () => ipcRenderer.send('close-window'),
  openExternal: (url) => ipcRenderer.send('open-external', url),
  onMaximizeChange: (callback) => {
    ipcRenderer.on('maximize-change', (event, isMaximized) => callback(isMaximized));
  },
  updateMinWidth: (width) => ipcRenderer.send('update-min-width', width),
  zoomIn: () => ipcRenderer.send('zoom-in'),
  zoomOut: () => ipcRenderer.send('zoom-out'),
  getSettings: () => ipcRenderer.invoke('get-app-settings'),
  saveSettings: (settings) => ipcRenderer.invoke('save-app-settings', settings),
  onSettingsUpdated: (callback) => {
    if (typeof callback !== 'function') {
      return undefined;
    }

    const channel = 'settings-updated';
    const handler = (_event, payload) => {
      callback(payload);
    };

    ipcRenderer.on(channel, handler);

    return () => {
      ipcRenderer.removeListener(channel, handler);
    };
  },
  getVersion: () => ipcRenderer.invoke('get-version'),
  discoverServerPort: () => ipcRenderer.invoke('discover-server-port'),
  getServerPort: () => ipcRenderer.invoke('get-server-port'),
  // Returns the configured server base URL (from --base-url or LEMONADE_APP_BASE_URL), or null if using localhost discovery
  getServerBaseUrl: () => ipcRenderer.invoke('get-server-base-url'),
  onServerPortUpdated: (callback) => {
    if (typeof callback !== 'function') {
      return undefined;
    }

    const channel = 'server-port-updated';
    const handler = (_event, port) => {
      callback(port);
    };

    ipcRenderer.on(channel, handler);

    return () => {
      ipcRenderer.removeListener(channel, handler);
    };
  },
  getSystemStats: () => ipcRenderer.invoke('get-system-stats'),
});

