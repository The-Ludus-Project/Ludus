// U1 UiText label qualification in Chromium on WebGPU and WebGL 2 (issue #1219).
// The page draws copied CPU glyph fixtures on Ui elements through UiText and checks every pixel
// against a composite of the snapped fills and atlas coverage under the element clips in Wasm; this
// harness checks the outcome, records backends, scale settings and batching counters, and requires
// byte-identical readbacks across backends. Native shaping, atlas exhaustion and device-loss cases
// run in the native SDK jobs. Contract: docs/architecture/ui.md#u1-text-adapter.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const root = resolve(process.argv[2]), output = resolve(process.argv[3] || 'out/ui-text-browser');
await mkdir(output, {recursive: true});
const server = createServer(async (request, response) => {
  const path = resolve(root, '.' + new URL(request.url, 'http://localhost').pathname);
  if (!path.startsWith(root + sep)) { response.writeHead(403).end(); return; }
  try { response.writeHead(200, {'Content-Type': path.endsWith('.wasm') ? 'application/wasm' : path.endsWith('.js') ? 'text/javascript' : 'text/html'}).end(await readFile(path)); }
  catch { response.writeHead(404).end(); }
});
await new Promise(done => server.listen(0, '127.0.0.1', done));
const args = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--use-vulkan=swiftshader', '--disable-vulkan-surface', '--enable-unsafe-swiftshader'];
const CASE_COUNT = 3;
const EXPECTED_CASES = [['1', '48', '32'], ['2', '96', '64'], ['2', '80', '60']];
const report = {kind: 'pinned Chromium software GPU; physical GPU qualification separate', cases: []};
let browser;
try {
  browser = await chromium.launch({headless: true, args});
  const pin = JSON.parse(await readFile(new URL('../../config/shader_toolchain.json', import.meta.url))).wgsl_validator;
  assert.equal(browser.version(), pin.chromium_version);
  report.version = browser.version();
  let oracle;
  for (const backend of ['webgpu', 'webgl2']) {
    const context = await browser.newContext({viewport: {width: 200, height: 160}, deviceScaleFactor: 1});
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
    await page.goto(`http://127.0.0.1:${server.address().port}/ui-text.html?backend=${backend}`);
    await page.waitForFunction(() => ['passed', 'failed'].includes(document.querySelector('#status').dataset.state), {}, {timeout: 30000});
    const state = await page.locator('#status').evaluate(node => ({...node.dataset}));
    assert.equal(state.state, 'passed', JSON.stringify({backend, state, errors}));
    assert.equal(state.backend, backend);
    assert.equal(state.cases, String(CASE_COUNT));
    assert.deepEqual(errors, []);
    const cases = [];
    for (let index = 0; index < CASE_COUNT; ++index) {
      const [scale, width, height, runs, textRuns, glyphs, culled, hash] = state[`case${index}`].split(',');
      assert.deepEqual([scale, width, height], EXPECTED_CASES[index], `${backend} case ${index} settings`);
      assert.equal(runs, '3'); assert.equal(textRuns, '1');
      assert.ok(Number(glyphs) > 0 && Number(culled) > 0, `${backend} case ${index} glyph counters`);
      cases.push({scale: Number(scale), width: Number(width), height: Number(height), runs: Number(runs), textRuns: Number(textRuns), glyphs: Number(glyphs), culledGlyphs: Number(culled), hash});
    }
    const observed = cases.map(entry => [entry.glyphs, entry.culledGlyphs, entry.hash]);
    if (oracle) assert.deepEqual(observed, oracle, 'UiText cross-backend counters and readback bytes');
    else oracle = observed;
    report.cases.push({backend, cases});
    await context.close();
  }
  await writeFile(resolve(output, 'report.json'), JSON.stringify(report, null, 2));
  console.log('U1 UiText: WebGPU and WebGL 2 draw clipped labels in paint order at scales 1 and 2 and after a resize, with byte-identical readbacks');
} finally { await browser?.close(); server.close(); }
