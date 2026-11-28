import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';

const root = path.dirname(fileURLToPath(import.meta.url));
const port = Number(process.env.PORT);
const basePath = process.env.BASE_PATH;

if (!Number.isInteger(port) || port <= 0) {
  throw new Error(`Invalid PORT value: "${process.env.PORT ?? ''}"`);
}

if (!basePath) {
  throw new Error('BASE_PATH environment variable is required but was not provided.');
}

export default defineConfig({
  base: basePath,
  root,
  build: {
    outDir: path.resolve(root, 'dist/public'),
    emptyOutDir: true,
  },
  server: {
    host: '0.0.0.0',
    port,
    strictPort: true,
    allowedHosts: true,
  },
  preview: {
    host: '0.0.0.0',
    port,
    allowedHosts: true,
  },
});
