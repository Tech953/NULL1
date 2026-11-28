import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readFile, stat } from 'node:fs/promises';

const root = path.dirname(fileURLToPath(import.meta.url));
const publicRoot = path.join(root, 'public');
const port = Number(process.env.PORT);

if (!Number.isInteger(port) || port <= 0) {
  throw new Error(`Invalid PORT value: "${process.env.PORT ?? ''}"`);
}

const contentTypes = new Map([
  ['.css', 'text/css; charset=utf-8'],
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.png', 'image/png'],
  ['.svg', 'image/svg+xml'],
  ['.txt', 'text/plain; charset=utf-8'],
  ['.zip', 'application/zip'],
]);

http
  .createServer(async (request, response) => {
    try {
      const url = new URL(request.url ?? '/', 'http://localhost');
      const pathname = decodeURIComponent(url.pathname);
      const requestedPath =
        pathname === '/'
          ? path.join(root, 'index.html')
          : path.resolve(publicRoot, `.${pathname}`);

      if (
        pathname !== '/' &&
        requestedPath !== publicRoot &&
        !requestedPath.startsWith(`${publicRoot}${path.sep}`)
      ) {
        response.writeHead(403).end('Forbidden');
        return;
      }

      const fileInfo = await stat(requestedPath);
      if (!fileInfo.isFile()) {
        response.writeHead(404).end('Not found');
        return;
      }

      response.writeHead(200, {
        'Content-Type':
          contentTypes.get(path.extname(requestedPath)) ??
          'application/octet-stream',
        'X-Content-Type-Options': 'nosniff',
      });
      response.end(await readFile(requestedPath));
    } catch {
      response.writeHead(404).end('Not found');
    }
  })
  .listen(port, '0.0.0.0');
