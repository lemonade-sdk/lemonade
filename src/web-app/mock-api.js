// The browser build has no Tauri host, so it installs the window.api the shared
// renderer expects. index.js imports this before the renderer so it is in place
// before any renderer module reads it.

window.api = {
    isWebApp: true,  // Explicit flag to indicate web mode
    platform: navigator.platform || 'web',
    minimizeWindow: () => {},
    maximizeWindow: () => {},
    closeWindow: () => {},
    openExternal: (url) => window.open(url, '_blank'),
    onMaximizeChange: () => {},
    updateMinWidth: () => {},
    zoomIn: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') + 0.1).toString(),
    zoomOut: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') - 0.1).toString(),
    getSettings: async () => {
        const saved = localStorage.getItem('lemonade-settings');
        if (saved) return JSON.parse(saved);
        // Return defaults matching DEFAULT_LAYOUT_SETTINGS from appSettings.ts
        return {
            layout: {
                isChatVisible: true,
                isModelManagerVisible: true,
                isCenterPanelVisible: true,
                isLogsVisible: false,
                modelManagerWidth: 280,
                chatWidth: 350,
                logsHeight: 200
            },
            theme: 'dark',
            apiUrl: window.location.origin,
            apiKey: { value: '' }
        };
    },
    saveSettings: async (settings) => {
        localStorage.setItem('lemonade-settings', JSON.stringify(settings));
        return settings;
    },
    onSettingsUpdated: () => {},
    getServerPort: () => parseInt(window.location.port) || 13305,
    onServerPortUpdated: () => {},
    getServerAPIKey: async () => {
        const settings = await window.api.getSettings();
        return settings.apiKey?.value || '';
    },
    restartApp: () => window.location.reload(),
    writeClipboard: async (text) => {
        if (navigator.clipboard) {
            try {
                await navigator.clipboard.writeText(text);
                return;
            } catch {
                // Ignore clipboard errors and fall back to legacy method
            }
        }
        const ta = document.createElement('textarea');
        ta.value = String(text);
        ta.setAttribute('readonly', '');
        ta.style.position = 'fixed';
        ta.style.left = '-9999px';
        document.body.appendChild(ta);
        try {
            ta.select();
            if (!document.execCommand('copy')) {
                throw new Error('Legacy clipboard copy failed');
            }
        } finally {
            document.body.removeChild(ta);
        }
    }
};
