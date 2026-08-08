import { defineConfig } from '@playwright/test';

const executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH;
const externalBaseURL = process.env.PLAYWRIGHT_BASE_URL?.trim();
const configuredPort = Number(process.env.PLAYWRIGHT_PORT || '4173');

if (!Number.isInteger(configuredPort) || configuredPort < 1 || configuredPort > 65535) {
  throw new Error(`Invalid PLAYWRIGHT_PORT: ${process.env.PLAYWRIGHT_PORT}`);
}

const baseURL = externalBaseURL || `http://127.0.0.1:${configuredPort}`;
const parsedBaseURL = new URL(baseURL);
const isLoopbackURL = ['127.0.0.1', 'localhost', '[::1]'].includes(parsedBaseURL.hostname);
const serverPort = parsedBaseURL.port
  ? Number(parsedBaseURL.port)
  : parsedBaseURL.protocol === 'https:'
    ? 443
    : 80;

if (!Number.isInteger(serverPort) || serverPort < 1 || serverPort > 65535) {
  throw new Error(`Invalid Playwright base URL port: ${baseURL}`);
}

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

  // A loopback PLAYWRIGHT_BASE_URL still needs a local renderer process.
  // Only genuinely external URLs opt out of Playwright-managed startup.
  ...(!externalBaseURL || isLoopbackURL ? {
    webServer: {
      command: `npm run dev:renderer -- --host 127.0.0.1 --port ${serverPort} --no-hot`,
      url: baseURL,
      reuseExistingServer: !process.env.CI,
      timeout: 120000,
    },
  } : {}),
});
