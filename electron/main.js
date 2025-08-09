const { app, BrowserWindow } = require('electron');
const path = require('path');
const { spawn } = require('child_process');

let serverProcess;

function createWindow() {
  const win = new BrowserWindow({
    width: 1024,
    height: 768,
    webPreferences: {
      nodeIntegration: false
    }
  });
  win.loadURL('http://localhost:8000/');
}

function startServer() {
  const installDir = path.dirname(app.getPath('exe'));
  const script = path.join(installDir, 'bin', 'lemonade-server.bat');
  serverProcess = spawn(script, ['serve', '--no-tray'], {
    cwd: installDir,
    shell: true,
    detached: false
  });
}

app.whenReady().then(() => {
  startServer();
  setTimeout(createWindow, 5000);
});

app.on('window-all-closed', () => {
  if (serverProcess) {
    serverProcess.kill();
  }
  if (process.platform !== 'darwin') {
    app.quit();
  }
});
