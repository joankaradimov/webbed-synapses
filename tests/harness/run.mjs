#!/usr/bin/env node
// Start a signalling server and run a program against it.
//
//     node tests/harness/run.mjs [--respawn SECONDS] [--http | --http-buffered]
//                                path/to/program [args...]
//
// The program is run as `program --server ws://127.0.0.1:<port> args...`; the
// harness, snp-sim and the wasm test all take that, a .mjs or .js under this
// same Node. Exits with the program's status.
//
// --http plays a host whose proxy will not upgrade a connection: the server
// refuses WebSockets, and the program is given an http:// url instead, so it
// has to find that out and fall back to HTTP sessions by itself.
// --http-buffered also holds every response back until it ends, as a proxy
// that buffers does, so the program has to fall back to long polling too.
// The server's log is shown only on failure, since that is when it is worth
// reading.
//
// The server's process id is in SNPS_SERVER_PID, for a program that wants to
// see what happens when the server dies. With --respawn, a server that dies is
// replaced, that many seconds later, by a new one on the same port with the
// same secret -- what an orchestrator does with a service it keeps running --
// and the program finds the delay in SNPS_SERVER_RESPAWN.
//
// The server is server/'s. It needs `npm ci` in server/ first.

import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { resolve } from 'node:path';

import { serverMissing, startServer } from './signal-server.mjs';

const usage = () => {
  process.stderr.write('usage: node tests/harness/run.mjs [--respawn SECONDS] [--http | --http-buffered] ' +
                       'path/to/program [args...]\n');
  process.exit(2);
};

const argv = process.argv.slice(2);
let respawn = null;
let mode = [];
let scheme = 'ws';
while (argv[0]?.startsWith('--')) {
  const option = argv.shift();
  if (option === '--respawn' && argv.length && Number.isFinite(Number(argv[0]))) {
    respawn = Number(argv.shift());
  } else if (option === '--http') {
    mode = ['--no-websocket'];
    scheme = 'http';
  } else if (option === '--http-buffered') {
    mode = ['--no-websocket', '--http-buffered'];
    scheme = 'http';
  } else {
    usage();
  }
}
const [target, ...extra] = argv;
if (!target)
  usage();
const program = resolve(target);
const command = /\.(m?js)$/.test(program) ? [process.execPath, program] : [program];

const missing = serverMissing();
if (missing) {
  process.stderr.write(`${missing}\n`);
  process.exit(1);
}

// The same for every server this run starts, so that a replacement honours
// the resume tokens its predecessor issued.
const secret = ['--secret', randomBytes(16).toString('hex'), ...mode];
const lines = [];

let server;
try {
  server = await startServer({ extra: secret, lines, scheme });
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exit(1);
}

let stopping = false;
let stop;
const stopped = new Promise(done => { stop = done; });
const pause = seconds => new Promise(done => setTimeout(done, seconds * 1000));

async function keepServing() {
  for (;;) {
    const how = await Promise.race([server.exited, stopped]);
    if (stopping)
      return;
    lines.push(`--- run.mjs: the server exited (${how}); another in ${respawn} s, on port ${server.port} ---`);
    await Promise.race([pause(respawn), stopped]);
    if (stopping)
      return;
    try {
      server = await startServer({ port: server.port, extra: secret, lines, scheme });
    } catch (error) {
      lines.push(`--- run.mjs: ${error.message} ---`);
      return;
    }
  }
}
const serving = respawn === null ? Promise.resolve() : keepServing();

let status = 1;
try {
  const env = { ...process.env, SNPS_SERVER_PID: String(server.pid) };
  if (respawn !== null)
    env.SNPS_SERVER_RESPAWN = String(respawn);
  const child = spawn(command[0], [...command.slice(1), '--server', server.url, ...extra],
                      { stdio: 'inherit', env });
  status = await new Promise(done => {
    child.once('error', error => {
      process.stderr.write(`cannot run ${program}: ${error.message}\n`);
      done(1);
    });
    child.once('exit', (code, signal) => done(code ?? (signal ? 1 : 0)));
  });
} finally {
  stopping = true;
  stop();
  await serving; // a replacement that was starting is `server` once this returns
  await server.stop();
  if (status !== 0)
    process.stderr.write(`\n--- signalling server, last 200 lines ---\n${lines.join('\n')}\n`);
}
process.exit(status);
