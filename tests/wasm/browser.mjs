#!/usr/bin/env node
// Set up the browser half of tests/wasm, then wait while a browser runs it.
//
//     node tests/wasm/browser.mjs <wasm build dir> <snps-echo> [--minutes 10]
//                                 [--stun stun:host:port] [--http | --http-buffered]
//
// Starts a signalling server (server/, as run.mjs does), the native echo peer
// (tests/harness's snps-echo), and a web server for the directory holding
// snps-wasm-test.js and test.html, then prints the page's address. Open it in
// a browser: the page ends with "passed" or "FAILED", and its title says the
// same. Ctrl+C stops everything.
//
// --http and --http-buffered play the hosts the HTTP form of signalling is
// for, as run.mjs does: WebSockets refused, and streams held back too. The
// page and the echo peer are given an http:// url and have to find out. The
// page is served from another port than the server, so this is the browser's
// fetch() across origins: CORS, which Node does not check.
//
// Near its end the page asks for /restart-signalling, and the signalling
// server is killed and replaced, two seconds later, by another on the same
// port with the same secret: the browser and the echo peer both have to come
// back to it.

import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { createWriteStream, existsSync, readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { parseArgs } from 'node:util';

import { serverMissing, startServer } from '../harness/signal-server.mjs';

const { values: args, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    minutes: { type: 'string', default: '10' },
    // A STUN server for the signalling server to hand out. Any address will
    // do, to exercise peer connections made with one.
    stun: { type: 'string' },
    http: { type: 'boolean', default: false },
    'http-buffered': { type: 'boolean', default: false },
  },
});
if (positionals.length !== 2) {
  process.stderr.write('usage: node tests/wasm/browser.mjs <wasm build dir> <snps-echo> [--minutes 10] [--stun url] ' +
                       '[--http | --http-buffered]\n');
  process.exit(2);
}
const wasmDir = resolve(positionals[0]);
const echoPath = resolve(positionals[1]);
const minutes = Number(args.minutes);

for (const name of ['snps-wasm-test.js', 'snps-wasm-test.wasm', 'test.html'])
  if (!existsSync(join(wasmDir, name))) {
    process.stderr.write(`${join(wasmDir, name)} is missing; build snps-wasm-test first\n`);
    process.exit(2);
  }
const missing = serverMissing();
if (missing) {
  process.stderr.write(`${missing}\n`);
  process.exit(1);
}

// The same for both servers, so the second honours the first one's tokens.
const serverArgs = ['--secret', randomBytes(16).toString('hex'), ...(args.stun ? ['--stun', args.stun] : []),
                    ...(args.http || args['http-buffered'] ? ['--no-websocket'] : []),
                    ...(args['http-buffered'] ? ['--http-buffered'] : [])];
const scheme = args.http || args['http-buffered'] ? 'http' : 'ws';

// The page, the program and its wasm; nothing outside the directory. And the
// one request that is not a file: replace the signalling server.
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const web = createServer((request, response) => {
  const url = new URL(request.url, 'http://x');
  if (url.pathname === '/restart-signalling') {
    response.writeHead(204).end();
    restartSignalling();
    return;
  }
  const path = normalize(join(wasmDir, decodeURIComponent(url.pathname)));
  if (!path.startsWith(wasmDir + sep) || !existsSync(path) || !TYPES[extname(path)]) {
    response.writeHead(404).end();
    return;
  }
  response.writeHead(200, { 'Content-Type': TYPES[extname(path)], 'Cache-Control': 'no-store' });
  response.end(readFileSync(path));
});

const log = createWriteStream(join(wasmDir, 'browser-test-server.log'));
let signalling = null;
let echo = null;
let stopping = false;
const stop = async () => {
  stopping = true;
  echo?.kill();
  await signalling?.stop();
  web.close();
  log.end();
};
process.on('SIGINT', () => stop().then(() => process.exit(0)));

async function restartSignalling() {
  const { port } = signalling;
  console.log('the page asked: killing the signalling server, and starting another in 2 s');
  await signalling.stop();
  await new Promise(done => setTimeout(done, 2000));
  if (stopping)
    return;
  try {
    signalling = await startServer({ port, log, extra: serverArgs, scheme });
    console.log(`signalling server back on port ${port}`);
  } catch (error) {
    console.log(`the signalling server did not come back: ${error.message}`);
  }
}

try {
  signalling = await startServer({ log, extra: serverArgs, scheme });
  echo = spawn(echoPath, ['--server', signalling.url, '--seconds', String(Math.round(minutes * 60))],
               { stdio: ['ignore', 'pipe', 'inherit'] });
  const echoed = new Promise(done => echo.once('exit', done));
  // The address only once the echo peer is there to answer the page.
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('snps-echo did not come online')), 10_000);
    echoed.then(code => reject(new Error(`snps-echo exited (${code}) before coming online`)));
    echo.stdout.on('data', chunk => {
      process.stdout.write(chunk);
      if (chunk.toString().includes(' online at ')) {
        clearTimeout(timer);
        resolve();
      }
    });
  });
  await new Promise(done => web.listen(0, '127.0.0.1', done));
  console.log(`open http://127.0.0.1:${web.address().port}/test.html?server=${signalling.url}&echo=1&restart=1`);
  await Promise.race([echoed, new Promise(done => setTimeout(done, minutes * 60_000))]);
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
} finally {
  await stop();
}
