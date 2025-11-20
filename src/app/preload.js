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
  readUserModels: () => ipcRenderer.invoke('read-user-models'),
  watchUserModels: (callback) => {
    if (typeof callback !== 'function') {
      return undefined;
    }

    const channel = 'user-models-updated';
    const handler = () => {
      callback();
    };

    ipcRenderer.on(channel, handler);
    ipcRenderer.send('start-watch-user-models');

    return () => {
      ipcRenderer.removeListener(channel, handler);
      ipcRenderer.send('stop-watch-user-models');
    };
  }
});

