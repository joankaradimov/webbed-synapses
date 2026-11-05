// A signalling server for a test: server/, run by this same Node, on a port of
// its own choosing. Used by run.mjs and tests/wasm/browser.mjs.
//
// The server is asked for port 0 and says which one it got, so there is no
// window between picking a free port and binding it for something else to
// take. A replacement, after the first one dies, asks for that same port.

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const SERVER = join(ROOT, 'server', 'src', 'main.js');
const KEEP_LINES = 200;

/** Why the server cannot start, if that can be told before trying. */
export function serverMissing() {
  if (!existsSync(join(ROOT, 'server', 'node_modules', 'ws')))
    return 'server/node_modules is missing: run `npm ci` in server/';
  return null;
}

/**
 * Starts the server on 127.0.0.1, logging every frame. Its output goes to
 * `log` (a writable stream) if given, and its last lines are kept in `lines`
 * either way -- pass the same array to several servers to keep one log of
 * all of them. Resolves once it is listening.
 */
export function startServer({ port = 0, extra = [], log = null, lines = [], timeoutMs = 10_000,
                              scheme = 'ws' } = {}) {
  const child = spawn(process.execPath,
                      [SERVER, '--host', '127.0.0.1', '--port', String(port), '--verbose', ...extra],
                      { stdio: ['ignore', 'pipe', 'pipe'] });
  let partial = '';
  let announce = null;

  // Read everything, always: a pipe nobody drains fills up, and then the
  // server blocks on its own logging and the test times out for nothing.
  const take = chunk => {
    log?.write(chunk);
    partial += chunk.toString('utf8');
    const complete = partial.split('\n');
    partial = complete.pop();
    for (const line of complete) {
      lines.push(line.replace(/\r$/, ''));
      if (lines.length > KEEP_LINES)
        lines.shift();
      const match = /listening on (?:ws|http):\/\/[^\s,]+:(\d+)/.exec(line);
      if (match)
        announce?.(Number(match[1]));
    }
  };
  child.stdout.on('data', take);
  child.stderr.on('data', take);
  // Its exit code, or the signal that ended it.
  const exited = new Promise(resolve => child.once('exit', (code, signal) => resolve(code ?? signal)));

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`the signalling server did not start:\n${lines.join('\n')}`));
    }, timeoutMs);
    child.once('error', error => {
      clearTimeout(timer);
      reject(new Error(`the signalling server did not start: ${error.message}`));
    });
    exited.then(code => {
      clearTimeout(timer);
      reject(new Error(`the signalling server exited (${code}) before listening:\n${lines.join('\n')}`));
    });
    announce = listening => {
      clearTimeout(timer);
      announce = null;
      resolve({
        url: `${scheme}://127.0.0.1:${listening}`,
        port: listening,
        pid: child.pid,
        exited,
        lines: () => [...lines, ...(partial ? [partial] : [])],
        async stop() {
          if (child.exitCode !== null || child.signalCode !== null)
            return;
          child.kill();
          const late = await Promise.race([exited.then(() => false),
                                           new Promise(done => setTimeout(done, 5000, true))]);
          if (late) {
            child.kill('SIGKILL');
            await exited;
          }
        },
      });
    };
  });
}
