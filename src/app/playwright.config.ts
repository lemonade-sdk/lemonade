import { defineConfig } from '@playwright/test';

const executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH;
const externalBaseURL = process.env.PLAYWRIGHT_BASE_URL;
const testPort = Number(process.env.PLAYWRIGHT_PORT || '4173');
const baseURL = externalBaseURL || `http://127.0.0.1:${testPort}`;
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
    trace: 'retain-on-failure',
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

  ...(externalBaseURL ? {} : {
    webServer: {
      command: `npm run dev -- --host 127.0.0.1 --port ${testPort} --no-hot`,
      url: baseURL,
      reuseExistingServer: false,
      timeout: 120000,
    },
  }),
});
