import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './test',
  testMatch: '*.spec.mjs',
  timeout: 120_000,
  workers: 1,
  use: {
    baseURL: 'http://127.0.0.1:5000',
    channel: process.env.P1S_BROWSER_CHANNEL || undefined,
  },
});
