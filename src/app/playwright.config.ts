import { defineConfig } from '@playwright/test';

const executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH;
const baseURL = process.env.PLAYWRIGHT_BASE_URL || 'http://localhost:8080';
const launchOptions = executablePath ? {
  executablePath,
  args: [
    '--no-sandbox',
    '--disable-setuid-sandbox',
    '--disable-dev-shm-usage',
    '--disable-features=BlockInsecurePrivateNetworkRequests',
  ],
} : undefined;

export default defineConfig({
  testDir: './tests',
  outputDir: './test-results',
  timeout: 60000,
  use: {
    baseURL,
    screenshot: 'on',
    trace: 'on-first-retry',
  },
  projects: [
    {
      name: 'chromium',
      use: {
        browserName: 'chromium',
        viewport: { width: 1280, height: 800 },
        ...(launchOptions ? { launchOptions } : {}),
      },
    },
  ],
  webServer: {
    // Playwright runs browser-only GUI tests. Start the webpack renderer server
    // directly instead of `tauri dev`, otherwise Linux CI tries to compile the
    // native Tauri host and needs GTK/GLib/WebKit development packages.
    command: 'npm run dev:renderer -- --port 8080',
    port: 8080,
    reuseExistingServer: true,
    timeout: 30000,
  },
});
