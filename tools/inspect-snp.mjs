#!/usr/bin/env node
// Read a .snp the way storm.dll reads it, and say whether it would be enumerated.
//
// Storm's failure mode is silence: a module whose archive it cannot parse is
// skipped with no error and never appears in the network menu. This walks the
// same path storm walks -- find the MPQ header by stepping 512 bytes at a time
// (SFILE.CPP:2088-2105), decrypt the hash and block tables, pull out
// `caps.dat`, and parse it into provider records the way SpiExtractCaps does
// (SNET.CPP:408-431) -- and reports what it finds.
//
//     node tools/inspect-snp.mjs build/msvc-x86/adapters/snp/null/RelWithDebInfo/null.snp
//     node tools/inspect-snp.mjs <diablo-hellfire>/src/Storm/SOURCE/STANDARD/CAPS.MPQ
//
// Deliberately independent of StormLib: a round trip through one library
// proves only that the library agrees with itself.
//
// Exits non-zero if storm would reject the file.

import { readFileSync, writeFileSync } from 'node:fs';
import { extname } from 'node:path';
import { parseArgs } from 'node:util';
import { inflateSync } from 'node:zlib';

const SIGNATURE = Buffer.from('MPQ\x1a', 'latin1');
const SEARCH_STEP = 512; // SFILE.CPP:2104
const HEADER_SIZE = 32;

const MPQ_FILE_IMPLODE = 0x00000100;
const MPQ_FILE_COMPRESS = 0x00000200;
const MPQ_FILE_ENCRYPTED = 0x00010000;
const MPQ_FILE_FIX_KEY = 0x00020000;
const MPQ_FILE_SINGLE_UNIT = 0x01000000;
const MPQ_FILE_EXISTS = 0x80000000;

const HASH_ENTRY_EMPTY = 0xFFFFFFFF;
const HASH_ENTRY_DELETED = 0xFFFFFFFE;

// Everything below is unsigned 32-bit arithmetic: `>>> 0` takes a sum modulo
// 2^32, and the shifts and XORs come out right modulo 2^32 too.

/** The MPQ key schedule. One table, filled by a fixed PRNG. */
const CRYPT = (() => {
  const table = new Uint32Array(0x500);
  let seed = 0x00100001;
  for (let i = 0; i < 0x100; ++i)
    for (let index = i, round = 0; round < 5; ++round, index += 0x100) {
      seed = (seed * 125 + 3) % 0x2AAAAB;
      const high = (seed & 0xFFFF) << 16;
      seed = (seed * 125 + 3) % 0x2AAAAB;
      table[index] = (high | (seed & 0xFFFF)) >>> 0;
    }
  return table;
})();

const HASH_TABLE_OFFSET = 0; // which of the five sub-tables a hash uses
const HASH_NAME_A = 1;
const HASH_NAME_B = 2;
const HASH_FILE_KEY = 3;

function hashString(text, kind) {
  let seed1 = 0x7FED7FED;
  let seed2 = 0xEEEEEEEE;
  for (const char of text.toUpperCase().replaceAll('/', '\\')) {
    const value = char.charCodeAt(0);
    seed1 = (CRYPT[(kind << 8) + value] ^ ((seed1 + seed2) >>> 0)) >>> 0;
    seed2 = (value + seed1 + seed2 + (seed2 << 5) + 3) >>> 0;
  }
  return seed1;
}

/** Decrypts whole 32-bit words; a ragged tail is left as it is, as storm does. */
function decrypt(data, key) {
  const out = Buffer.from(data);
  let seed1 = key >>> 0;
  let seed2 = 0xEEEEEEEE;
  for (let offset = 0; offset + 4 <= out.length; offset += 4) {
    seed2 = (seed2 + CRYPT[0x400 + (seed1 & 0xFF)]) >>> 0;
    const value = (out.readUInt32LE(offset) ^ ((seed1 + seed2) >>> 0)) >>> 0;
    seed1 = (((~seed1 << 21) + 0x11111111) | (seed1 >>> 11)) >>> 0;
    seed2 = (value + seed2 + (seed2 << 5) + 3) >>> 0;
    out.writeUInt32LE(value, offset);
  }
  return out;
}

class NotSupported extends Error {}

class Archive {
  constructor(blob, base) {
    this.blob = blob;
    this.base = base;
    this.headerSize = blob.readUInt32LE(base + 4);
    this.archiveSize = blob.readUInt32LE(base + 8);
    this.version = blob.readUInt16LE(base + 12);
    this.sectorShift = blob.readUInt16LE(base + 14);
    const hashPos = blob.readUInt32LE(base + 16);
    const blockPos = blob.readUInt32LE(base + 20);
    this.hashCount = blob.readUInt32LE(base + 24);
    this.blockCount = blob.readUInt32LE(base + 28);
    this.hashTable = decrypt(blob.subarray(base + hashPos, base + hashPos + this.hashCount * 16),
                             hashString('(hash table)', HASH_FILE_KEY));
    this.blockTable = decrypt(blob.subarray(base + blockPos, base + blockPos + this.blockCount * 16),
                              hashString('(block table)', HASH_FILE_KEY));
  }

  get sectorSize() { return 512 << this.sectorShift; }

  /** [offset, packed size, size, flags] for a name, or null. */
  find(name) {
    const mask = this.hashCount - 1;
    const start = hashString(name, HASH_TABLE_OFFSET) & mask;
    const wantA = hashString(name, HASH_NAME_A);
    const wantB = hashString(name, HASH_NAME_B);
    for (let step = 0; step < this.hashCount; ++step) {
      const at = ((start + step) & mask) * 16;
      if (at + 16 > this.hashTable.length)
        return null;
      const block = this.hashTable.readUInt32LE(at + 12);
      if (block === HASH_ENTRY_EMPTY)
        return null;
      if (block === HASH_ENTRY_DELETED)
        continue;
      if (this.hashTable.readUInt32LE(at) === wantA && this.hashTable.readUInt32LE(at + 4) === wantB) {
        if (block >= this.blockCount || block * 16 + 16 > this.blockTable.length)
          return null;
        return [0, 4, 8, 12].map(field => this.blockTable.readUInt32LE(block * 16 + field));
      }
    }
    return null;
  }

  read(name) {
    const entry = this.find(name);
    if (!entry || !(entry[3] & MPQ_FILE_EXISTS))
      throw new Error(`${name} is not in the archive`);
    const [position, , size, flags] = entry;

    const start = this.base + position;
    if (!(flags & (MPQ_FILE_COMPRESS | MPQ_FILE_IMPLODE)))
      // What snp-mpq writes, and the simplest thing storm can read.
      return this.blob.subarray(start, start + size);

    // Blizzard's own archives are compressed and encrypted, so the rest of
    // this exists to read theirs -- ours never takes this path.
    if (flags & MPQ_FILE_IMPLODE)
      throw new NotSupported('PKWARE DCL imploded; no decompressor here');
    if (flags & MPQ_FILE_SINGLE_UNIT)
      throw new NotSupported('single-unit compressed file');

    let key = 0;
    if (flags & MPQ_FILE_ENCRYPTED) {
      key = hashString(name.split('\\').pop(), HASH_FILE_KEY);
      if (flags & MPQ_FILE_FIX_KEY)
        key = ((key + position) ^ size) >>> 0;
    }

    const sectors = Math.ceil(size / this.sectorSize);
    let table = this.blob.subarray(start, start + (sectors + 1) * 4);
    if (flags & MPQ_FILE_ENCRYPTED)
      table = decrypt(table, (key - 1) >>> 0);
    const offsets = Array.from({ length: sectors + 1 }, (_, i) => table.readUInt32LE(i * 4));

    const out = [];
    let length = 0;
    for (let index = 0; index < sectors; ++index) {
      let chunk = this.blob.subarray(start + offsets[index], start + offsets[index + 1]);
      if (flags & MPQ_FILE_ENCRYPTED)
        chunk = decrypt(chunk, (key + index) >>> 0);
      const plainSize = Math.min(this.sectorSize, size - length);
      if (chunk.length < plainSize) {
        const mask = chunk[0];
        if (mask !== 0x02)
          throw new NotSupported(`sector compressed with mask 0x${mask.toString(16).padStart(2, '0')}`);
        chunk = inflateSync(chunk.subarray(1));
      }
      out.push(chunk.subarray(0, plainSize));
      length += Math.min(chunk.length, plainSize);
    }
    return Buffer.concat(out);
  }
}

function openArchive(path) {
  const blob = readFileSync(path);
  for (let offset = 0; offset + HEADER_SIZE <= blob.length; offset += SEARCH_STEP)
    if (blob.subarray(offset, offset + 4).equals(SIGNATURE))
      return new Archive(blob, offset);
  throw new Error('no MPQ header on a 512-byte boundary: storm would skip this file without a word');
}

/** Walks caps.dat the way SpiExtractCaps does, record by record. */
function* parseCaps(data) {
  const string = from => {
    const end = data.indexOf(0, from);
    if (end < 0)
      throw new Error(`caps.dat: a string at ${from} has no terminating NUL`);
    return [data.toString('latin1', from, end), end + 1];
  };
  for (let at = 0; at < data.length;) {
    const declared = data.readUInt32LE(at);
    const start = at + 4;
    const id = Buffer.from(data.subarray(start, start + 4)).reverse().toString('latin1');
    let cursor = start + 4;
    let description, requirements;
    [description, cursor] = string(cursor);
    [requirements, cursor] = string(cursor);
    const capsSize = data.readUInt32LE(cursor);
    const caps = Array.from({ length: 9 }, (_, i) => data.readUInt32LE(cursor + i * 4));
    cursor += capsSize;
    yield { id, description, requirements, caps, declared, actual: cursor - start };
    at = cursor;
  }
}

const CAPS_FIELDS = ['size', 'flags', 'maxmessagesize', 'maxqueuesize', 'maxplayers',
                     'bytessec', 'latencyms', 'defaultturnssec', 'defaultturnsintransit'];

/** A string quoted, its escapes visible, as this tool has always printed them. */
function quoted(text) {
  const quote = text.includes("'") && !text.includes('"') ? '"' : "'";
  let out = quote;
  for (const char of text) {
    const code = char.codePointAt(0);
    if (char === '\\' || char === quote) out += `\\${char}`;
    else if (char === '\n') out += '\\n';
    else if (char === '\r') out += '\\r';
    else if (char === '\t') out += '\\t';
    else if (code < 0x20 || (code >= 0x7F && code <= 0xA0) || code === 0xAD)
      out += `\\x${code.toString(16).padStart(2, '0')}`;
    else out += char;
  }
  return out + quote;
}

function main() {
  const { values: args, positionals } = parseArgs({
    allowPositionals: true,
    options: { extract: { type: 'string' } }, // write caps.dat out to this path
  });
  if (positionals.length !== 1) {
    process.stderr.write('usage: node tools/inspect-snp.mjs <module.snp | archive.mpq | caps.dat> [--extract FILE]\n');
    return 2;
  }
  const path = positionals[0];
  const problems = [];
  let data;

  if (extname(path).toLowerCase() === '.dat') {
    // Handy for checking a generated caps.dat before it is packed.
    data = readFileSync(path);
    console.log(`${path}: ${data.length} bytes of raw caps data`);
  } else {
    const archive = openArchive(path);
    console.log(path);
    console.log(`  archive at offset ${archive.base} (${archive.base % 512 === 0 ? '512-byte aligned' : 'MISALIGNED'})`);
    console.log(`  MPQ v${archive.version + 1}, ${archive.sectorSize}-byte sectors, ` +
                `${archive.hashCount} hash slots, ${archive.blockCount} files, ${archive.archiveSize} bytes`);
    if (archive.version !== 0)
      problems.push('archive is not MPQ v1');

    const entry = archive.find('caps.dat');
    if (!entry) {
      console.log('  caps.dat: ABSENT -- storm skips this module');
      return 1;
    }
    const [, packed, size, flags] = entry;
    console.log(`  caps.dat: ${size} bytes, flags 0x${flags.toString(16).padStart(8, '0')}` +
                (!(flags & (MPQ_FILE_COMPRESS | MPQ_FILE_IMPLODE))
                  ? ' (stored)' : ` (compressed, ${packed} bytes packed)`));
    try {
      data = archive.read('caps.dat');
    } catch (error) {
      if (!(error instanceof NotSupported))
        throw error;
      console.log(`  cannot read it here: ${error.message}`);
      return 0;
    }
    if (args.extract) {
      writeFileSync(args.extract, data);
      console.log(`  extracted to ${args.extract}`);
    }
  }

  console.log();
  let index = 0;
  for (const record of parseCaps(data)) {
    const ok = record.declared === record.actual;
    if (!ok)
      problems.push(`record ${index} declares ${record.declared} bytes but measures ${record.actual}`);
    console.log(`  provider ${index}: '${record.id}'  ${ok ? 'ok' : 'LENGTH MISMATCH'}`);
    console.log(`    description  ${quoted(record.description)}`);
    console.log(`    requirements ${quoted(record.requirements)}`);
    CAPS_FIELDS.forEach((name, i) => console.log(
      `    ${name.padEnd(22)}${record.caps[i]}` +
      (name === 'maxmessagesize' ? '  (storm shows this minus 16)' : '')));
    ++index;
  }

  if (problems.length) {
    console.log(`\n${problems.map(p => `  problem: ${p}`).join('\n')}`);
    return 1;
  }
  console.log('\nstorm would accept this.');
  return 0;
}

try {
  process.exitCode = main();
} catch (error) {
  process.stderr.write(`inspect-snp: ${error.message}\n`);
  process.exitCode = 1;
}
