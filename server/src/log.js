// The server's log: timestamped lines on stderr, and debug lines only when
// asked for. Debug includes every frame in and out (truncated), which is how
// a failed negotiation gets diagnosed.

const LEVELS = { debug: 10, info: 20, warning: 30, error: 40 };
let threshold = LEVELS.info;

function stamp() {
  const now = new Date();
  const pad = (n, width = 2) => String(n).padStart(width, '0');
  return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ` +
         `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}.` +
         `${pad(now.getMilliseconds(), 3)}`;
}

function write(level, text) {
  if (LEVELS[level] < threshold)
    return;
  process.stderr.write(`${stamp()} ${level.toUpperCase().padEnd(7)} ${text}\n`);
}

export const log = {
  debug: text => write('debug', text),
  info: text => write('info', text),
  warning: text => write('warning', text),
  error: text => write('error', text),
  verbose: on => { threshold = on ? LEVELS.debug : LEVELS.info; },
  quiet: () => { threshold = Infinity; },
};
