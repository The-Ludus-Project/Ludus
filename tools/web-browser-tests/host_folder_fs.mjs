// Test-only checks for apps/editor/browser/host_folder.js through the Wasm harness
// in apps/editor/tests/browser/host_folder_harness.cpp. Mock directory handles stand
// in for the File System Access API over a temporary folder, so the test sees the
// bytes that reach "disk". Usage: node host_folder_fs.mjs <harness.mjs>
import assert from 'node:assert/strict';
import {mkdtemp, mkdir, readFile, readdir, rm, stat, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const harnessPath = resolve(process.argv[2] || '../../out/editor-host-folder-test/host_folder_harness.mjs');
const reads = [];
let failWrites = null;

class MockFileHandle {
  constructor(path, name) { this.kind = 'file'; this.path = path; this.name = name; }
  async getFile() {
    const path = this.path;
    const info = await stat(path);
    return {
      name: this.name, size: info.size, lastModified: info.mtimeMs,
      async arrayBuffer() {
        reads.push(path);
        const bytes = await readFile(path);
        return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      },
    };
  }
  async createWritable() {
    if (failWrites !== null && this.path.endsWith(failWrites)) {
      throw Object.assign(new Error('mock disk refused the write'), {name: 'NoModificationAllowedError'});
    }
    const chunks = [];
    const path = this.path;
    return {
      async write(bytes) { chunks.push(Buffer.from(bytes)); },
      async close() { await writeFile(path, Buffer.concat(chunks)); },
    };
  }
}

class MockDirectoryHandle {
  constructor(path, name) { this.kind = 'directory'; this.path = path; this.name = name; }
  async *values() {
    for (const entry of await readdir(this.path, {withFileTypes: true})) {
      const path = join(this.path, entry.name);
      yield entry.isDirectory() ? new MockDirectoryHandle(path, entry.name) : new MockFileHandle(path, entry.name);
    }
  }
  async getFileHandle(name, options = {}) {
    const path = join(this.path, name);
    try {
      await stat(path);
    } catch {
      if (!options.create) throw Object.assign(new Error(name), {name: 'NotFoundError'});
      await writeFile(path, '');
    }
    return new MockFileHandle(path, name);
  }
  async getDirectoryHandle(name, options = {}) {
    const path = join(this.path, name);
    try {
      await stat(path);
    } catch {
      if (!options.create) throw Object.assign(new Error(name), {name: 'NotFoundError'});
      await mkdir(path);
    }
    return new MockDirectoryHandle(path, name);
  }
  async removeEntry(name, options = {}) {
    const path = join(this.path, name);
    try {
      await stat(path);
    } catch {
      throw Object.assign(new Error(name), {name: 'NotFoundError'});
    }
    await rm(path, {recursive: Boolean(options.recursive)});
  }
  async queryPermission() { return 'granted'; }
  async requestPermission() { return 'granted'; }
}

const work = await mkdtemp(join(tmpdir(), 'ludus-host-folder-'));
const folder = join(work, 'Project');
await mkdir(join(folder, 'assets'), {recursive: true});
await mkdir(join(folder, 'out/build'), {recursive: true});
await writeFile(join(folder, 'ludus.project.json'), '{"version": 2}\n');
await writeFile(join(folder, 'assets/large.bin'), Buffer.alloc(3000, 7));
await writeFile(join(folder, 'assets/other.bin'), Buffer.alloc(3000, 9));
await writeFile(join(folder, 'out/build/ignored.o'), 'generated');

globalThis.showDirectoryPicker = async () => new MockDirectoryHandle(folder, 'Project');
const Module = await (await import(pathToFileURL(harnessPath))).default();
const call = (name, returns, types, values) => Module.ccall(name, returns, types, values, {async: true});
const text = value => new TextEncoder().encode(value);

async function pick() {
  const picked = new Promise((done, failed) => {
    setTimeout(() => {
      globalThis.harnessTarget.addEventListener('ludushostpicked', event => done(event.detail));
      globalThis.harnessTarget.addEventListener('ludushostfailed', event => failed(new Error(JSON.stringify(event.detail))));
    }, 0);
  });
  Module._HarnessPick(0);
  return await picked;
}

async function readText(path, capacity = 8192) {
  const buffer = Module._malloc(capacity);
  try {
    const count = await call('HarnessRead', 'number', ['string', 'number', 'number'], [path, buffer, capacity]);
    return count < 0 ? count : Buffer.from(Module.HEAPU8.subarray(buffer, buffer + count));
  } finally {
    Module._free(buffer);
  }
}

async function save(name, path, value) {
  const bytes = text(value);
  const buffer = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, buffer);
  try {
    return await call(name, 'number', ['string', 'number', 'number'], [path, buffer, bytes.length]);
  } finally {
    Module._free(buffer);
  }
}

const settle = () => new Promise(done => setTimeout(done, 50));
const root = '/browser-folders/1/Project';
try {
  // Enumeration skips generated folders and reads no bytes.
  const selection = await pick();
  assert.equal(selection.writable, true);
  assert.deepEqual(selection.entries.map(entry => entry.path).sort(),
    ['assets/large.bin', 'assets/other.bin', 'ludus.project.json']);
  assert.equal(reads.length, 0, 'enumeration must not read file bytes');
  // A 4000-byte budget holds one 3000-byte asset at a time.
  assert.equal(await call('HarnessMount', 'number', ['number', 'string', 'number'], [selection.selection, '/browser-folders/1', 4000]), 0);
  assert.equal(Number(await call('HarnessSize', 'number', ['string'], [root + '/assets/large.bin'])), 3000);
  assert.equal(reads.length, 0, 'stat must not read file bytes');

  // First open reads from disk; a second open reuses the resident copy.
  assert.equal((await readText(root + '/ludus.project.json')).toString(), '{"version": 2}\n');
  assert.equal(reads.length, 1);
  await readText(root + '/ludus.project.json');
  assert.equal(reads.length, 1, 'resident files are not read again');

  // The budget evicts the clean, closed copy of large.bin to admit other.bin.
  assert.equal((await readText(root + '/assets/large.bin')).length, 3000);
  assert.equal((await readText(root + '/assets/other.bin')).length, 3000);
  assert.equal((await readText(root + '/assets/large.bin'))[0], 7);
  assert.equal(reads.filter(path => path.endsWith('large.bin')).length, 2, 'evicted copies are read again');

  // A QSaveFile-style save reaches disk before rename() returns, without its temporary file.
  assert.equal(await save('HarnessSave', root + '/ludus.project.json', '{"version": 3}\n'), 0);
  assert.equal(await readFile(join(folder, 'ludus.project.json'), 'utf8'), '{"version": 3}\n');
  await settle();
  assert.deepEqual((await readdir(folder)).sort(), ['assets', 'ludus.project.json', 'out']);

  // Plain writes and new folders reach disk after close(), on the next turn.
  assert.equal(await call('HarnessMakeDirectory', 'number', ['string'], [root + '/worlds']), 0);
  assert.equal(await save('HarnessWrite', root + '/worlds/new.world.json', '{}'), 0);
  await settle();
  assert.equal(await readFile(join(folder, 'worlds/new.world.json'), 'utf8'), '{}');

  // Renaming a file that was never read copies it from its old place on disk.
  const before = reads.length;
  assert.equal(await call('HarnessRename', 'number', ['string', 'string'], [root + '/assets/other.bin', root + '/assets/moved.bin']), 0);
  assert.equal((await readFile(join(folder, 'assets/moved.bin'))).length, 3000);
  await assert.rejects(stat(join(folder, 'assets/other.bin')));
  assert.ok(reads.length >= before);

  // Deleting reaches disk; generated folders stay session-only.
  assert.equal(await call('HarnessUnlink', 'number', ['string'], [root + '/assets/moved.bin']), 0);
  assert.equal(await call('HarnessMakeDirectory', 'number', ['string'], [root + '/out']), 0);
  assert.equal(await save('HarnessWrite', root + '/out/session.bin', 'cache'), 0);
  await settle();
  await assert.rejects(stat(join(folder, 'assets/moved.bin')));
  await assert.rejects(stat(join(folder, 'out/session.bin')));

  // A refused disk write fails the save and is retried by the next one.
  failWrites = 'ludus.project.json';
  assert.equal(await save('HarnessSave', root + '/ludus.project.json', '{"version": 4}\n'), -29);
  assert.equal(await readFile(join(folder, 'ludus.project.json'), 'utf8'), '{"version": 3}\n');
  assert.equal((await readText(root + '/ludus.project.json')).toString(), '{"version": 4}\n', 'the session keeps the edit');
  failWrites = null;
  assert.equal(await save('HarnessSave', root + '/ludus.project.json', '{"version": 5}\n'), 0);
  assert.equal(await readFile(join(folder, 'ludus.project.json'), 'utf8'), '{"version": 5}\n');


  // Without showDirectoryPicker (Firefox, Safari) the directory <input> lists File
  // objects: still read lazily, but read-only, so edits stay in the session.
  delete globalThis.showDirectoryPicker;
  const fallbackReads = [];
  const fakeFile = (path, value) => ({
    name: path.split('/').pop(), webkitRelativePath: path, size: value.length,
    async arrayBuffer() { fallbackReads.push(path); return new TextEncoder().encode(value).buffer; },
  });
  const input = new EventTarget();
  input.remove = () => {};
  input.click = () => setTimeout(() => {
    input.files = [fakeFile('Copy/ludus.project.json', '{}'), fakeFile('Copy/assets/a.txt', 'alpha'),
      fakeFile('Copy/out/b.o', 'generated')];
    input.dispatchEvent(new Event('change'));
  }, 0);
  globalThis.document = {createElement: () => input, body: {appendChild() {}}};
  const fallback = await pick();
  assert.equal(fallback.writable, false);
  assert.deepEqual(fallback.entries.map(entry => entry.path).sort(), ['assets/a.txt', 'ludus.project.json']);
  assert.equal(await call('HarnessMount', 'number', ['number', 'string', 'number'], [fallback.selection, '/browser-folders/2', 4000]), 0);
  assert.equal(fallbackReads.length, 0);
  assert.equal((await readText('/browser-folders/2/Copy/assets/a.txt')).toString(), 'alpha');
  assert.deepEqual(fallbackReads, ['Copy/assets/a.txt']);
  assert.equal(await save('HarnessSave', '/browser-folders/2/Copy/assets/a.txt', 'beta'), 0);
  assert.equal((await readText('/browser-folders/2/Copy/assets/a.txt')).toString(), 'beta');
  console.log('host_folder_fs: all checks passed');
} finally {
  await rm(work, {recursive: true, force: true});
}
