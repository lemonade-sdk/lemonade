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
  updateMinWidth: (width) => ipcRenderer.send('update-min-width', width)
});

