// Test-only acceptance for project folders opened in place in the Qt/Wasm editor
// (apps/editor/browser/host_folder.js). Chromium's origin-private file system
// supplies a real FileSystemDirectoryHandle through a stubbed showDirectoryPicker,
// so the test reads back what the editor saved without a native folder dialog.
//
// Usage: node editor_host_folder.mjs <editor-pages> <output> [project-folder]
// Without a project folder a fixture project with a 40 MiB asset is used (larger
// than the former 32 MiB session total). With one (for example Dolomites Survival)
// its files outside out/, build/, .git/, .ludus/ and node_modules/ are copied into
// the origin-private file system first.
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {mkdir, readFile, readdir, writeFile} from 'node:fs/promises';
import {join, relative, resolve, sep} from 'node:path';
import {chromium} from 'playwright';

const root = resolve(process.argv[2] || '../../out/editor-pages');
const output = resolve(process.argv[3] || '../../out/editor-host-folder-qa');
const projectFolder = process.argv[4] ? resolve(process.argv[4]) : null;
const generated = ['out', 'build', '.git', '.ludus', 'node_modules'];
const MEBIBYTE = 1024 * 1024;
await mkdir(output, {recursive: true});
const report = {cases: [], errors: [], console: []};

async function listProject(folder, prefix = '') {
  const files = [];
  for (const entry of await readdir(join(folder, prefix), {withFileTypes: true})) {
    const path = prefix ? prefix + '/' + entry.name : entry.name;
    if (prefix === '' && generated.includes(entry.name)) continue;
    if (entry.isDirectory()) files.push(...await listProject(folder, path));
    else if (entry.isFile()) files.push(path);
  }
  return files;
}
const projectFiles = projectFolder ? await listProject(projectFolder) : [];

const server = createServer(async (request, response) => {
  const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
  const [base, prefix] = pathname.startsWith('/qa-project/') && projectFolder
    ? [projectFolder, '/qa-project/'] : [root, '/'];
  const file = resolve(base, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname).slice(prefix.length - 1));
  if (!file.startsWith(base + sep)) { response.writeHead(403).end(); return; }
  try {
    const bytes = await readFile(file);
    const type = file.endsWith('.wasm') ? 'application/wasm' : file.endsWith('.js') ? 'text/javascript'
      : file.endsWith('.css') ? 'text/css' : file.endsWith('.html') ? 'text/html' : 'application/octet-stream';
    response.writeHead(200, {'Content-Type': type, 'Cache-Control': 'no-store'}).end(bytes);
  } catch { response.writeHead(404).end(); }
});
await new Promise(done => server.listen(0, '127.0.0.1', done));
const base = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({executablePath: process.env.LUDUS_QA_BROWSER || undefined,
  args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader']});
report.browser = browser.version();

async function click(page, x, y) {
  await page.mouse.move(x, y);
  await page.waitForTimeout(100);
  await page.mouse.click(x, y, {delay: 50});
  await page.waitForTimeout(200);
}

try {
  const page = await browser.newPage({viewport: {width: 1280, height: 800}});
  page.on('pageerror', error => report.errors.push(error.message));
  page.on('console', message => report.console.push(message.text()));
  // The picker returns an origin-private folder; Chromium handles have the same API as disk folders.
  await page.addInitScript(() => {
    // Count the file bytes the editor reads, to show that opening reads only what it needs.
    globalThis.qaBytesRead = 0;
    const arrayBuffer = Blob.prototype.arrayBuffer;
    Blob.prototype.arrayBuffer = function() {
      globalThis.qaBytesRead += this.size;
      return arrayBuffer.call(this);
    };
    globalThis.showDirectoryPicker = async () => {
      const opfs = await navigator.storage.getDirectory();
      return await opfs.getDirectoryHandle(globalThis.qaFolderName);
    };
  });
  await page.goto(base + '/', {waitUntil: 'domcontentloaded'});
  await page.waitForFunction(() => document.body.dataset.editorState === 'ready', null, {timeout: 60000});
  const keymapChooser = page.getByText('Choose a keymap', {exact: true});
  await keymapChooser.waitFor({timeout: 30000});
  await page.keyboard.press('Enter', {delay: 50});
  await keymapChooser.waitFor({state: 'hidden', timeout: 10000});

  // Populate the origin-private folder.
  const folderName = projectFolder ? projectFolder.split(sep).at(-1) : 'Folder Fixture';
  const started = Date.now();
  await page.evaluate(async ({folderName, files}) => {
    globalThis.qaFolderName = folderName;
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry(folderName, {recursive: true}).catch(() => {});
    const folder = await opfs.getDirectoryHandle(folderName, {create: true});
    const write = async (path, bytes) => {
      let directory = folder;
      const parts = path.split('/');
      for (const part of parts.slice(0, -1)) directory = await directory.getDirectoryHandle(part, {create: true});
      const writable = await (await directory.getFileHandle(parts.at(-1), {create: true})).createWritable();
      await writable.write(bytes);
      await writable.close();
    };
    if (files.length === 0) {
      await write('ludus.project.json', JSON.stringify({version: 1, name: 'Folder Fixture', provider: 'cmake',
        source_dir: '.', preset: 'linux-clang-development', target: 'fixture', run: {cwd: '.', args: []}}, null, 2));
      await write('assets/large.bin', new Uint8Array(40 * 1024 * 1024).fill(7));
      await write('src/main.cpp', 'int main() { return 0; }\n');
    }
    for (const path of files) {
      await write(path, await (await fetch('/qa-project/' + path.split('/').map(encodeURIComponent).join('/'))).arrayBuffer());
    }
  }, {folderName, files: projectFiles});
  report.copySeconds = (Date.now() - started) / 1000;
  report.folderBytes = await page.evaluate(() => globalThis.qaBytesRead);
  await page.evaluate(() => { globalThis.qaBytesRead = 0; });
  report.files = projectFiles.length || 3;

  // File → Open Project... (the Open toolbar button) uses showDirectoryPicker and mounts the folder in place.
  await click(page, 54, 157);
  await page.waitForFunction(() => document.title.includes('ludus.project.json') ||
    [...document.querySelectorAll('*')].some(node => node.textContent === 'Browser folders'), null, {timeout: 30000})
    .catch(() => {});
  await page.waitForTimeout(3000);
  await page.screenshot({path: resolve(output, 'opened.png')});
  report.title = await page.title();
  report.bytesReadByOpen = await page.evaluate(() => globalThis.qaBytesRead);
  report.cases.push('Open Project mounts a Chromium directory handle in place');

  // Edit the project name in the Project tab and save (Ctrl+S): the save must reach the folder.
  const edited = 'Edited in the browser';
  await click(page, 302, 195); // Project Settings tab.
  await page.waitForTimeout(500);
  await page.screenshot({path: resolve(output, 'settings.png')});
  const nameField = JSON.parse(process.env.LUDUS_QA_NAME_FIELD || '[660, 490]');
  await click(page, nameField[0], nameField[1]);
  await page.keyboard.press('ControlOrMeta+A');
  await page.keyboard.type(edited, {delay: 10});
  await click(page, 142, 157); // Save toolbar button.
  await page.waitForFunction(async folderName => {
    const folder = await (await navigator.storage.getDirectory()).getDirectoryHandle(folderName);
    const text = await (await (await folder.getFileHandle('ludus.project.json')).getFile()).text();
    return text.includes('Edited in the browser');
  }, folderName, {timeout: 15000}).catch(() => {});
  await page.waitForTimeout(2000);
  await page.screenshot({path: resolve(output, 'saved.png')});
  const saved = await page.evaluate(async folderName => {
    const folder = await (await navigator.storage.getDirectory()).getDirectoryHandle(folderName);
    return await (await (await folder.getFileHandle('ludus.project.json')).getFile()).text();
  }, folderName);
  report.savedDescriptor = saved;
  assert.equal(JSON.parse(saved).name, edited, 'the saved name reached the folder');
  report.cases.push('Save writes the edited descriptor back to the folder');
  report.pendingWrites = await page.evaluate(() => globalThis.ludusHostPendingWrites?.());
  // Optionally open a World from the Project Explorer: its assets are read from the folder on demand.
  if (process.env.LUDUS_QA_OPEN_WORLD) {
    const [x, y] = JSON.parse(process.env.LUDUS_QA_OPEN_WORLD);
    await page.mouse.dblclick(x, y, {delay: 50});
    await page.waitForTimeout(Number(process.env.LUDUS_QA_WORLD_WAIT_MS || 20000));
    await page.screenshot({path: resolve(output, 'world.png')});
    report.bytesReadByWorld = await page.evaluate(() => globalThis.qaBytesRead);
    report.cases.push('Opening a World reads its assets from the folder on demand');
  }
  assert.deepEqual(report.errors, []);
  report.passed = true;
} catch (error) {
  report.failure = String(error?.stack || error);
  process.exitCode = 1;
} finally {
  await writeFile(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  await browser.close();
  server.close();
}
console.log(JSON.stringify({passed: report.passed === true, cases: report.cases, failure: report.failure}, null, 2));
