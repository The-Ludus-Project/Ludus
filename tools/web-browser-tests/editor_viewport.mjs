// Test-only acceptance for the actual Qt/Wasm editor and engine WebGPU renderer.
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {resolve, sep} from 'node:path';
import {chromium} from 'playwright';
import {PNG} from 'pngjs';

const root = resolve(process.argv[2] || '../../out/editor-pages');
const output = resolve(process.argv[3] || '../../out/editor-viewport-qa');
await mkdir(output, {recursive: true});
const launchArgs = process.env.LUDUS_QA_GPU === 'hardware'
  ? ['--enable-unsafe-webgpu', '--no-proxy-server', ...(process.platform === 'darwin' ? ['--use-angle=metal'] : [])]
  : ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader',
    '--use-angle=swiftshader', '--use-vulkan=swiftshader', '--disable-vulkan-surface', '--enable-unsafe-swiftshader'];
const report = {kind: process.env.LUDUS_QA_GPU || 'software-GPU', launchArgs, cases: [], errors: []};
const source = Buffer.from(`LookAt 0 0 -4 0 0 0 0 1 0
Camera "perspective" "float fov" [45]
WorldBegin
LightSource "infinite" "rgb L" [0.2 0.2 0.2]
LightSource "distant" "point3 from" [0 2 -2] "point3 to" [0 0 0] "rgb L" [2 2 2]
Material "diffuse" "rgb reflectance" [0.8 0.15 0.05]
Shape "trianglemesh" "point3 P" [-1 -1 0 1 -1 0 0 1 0] "integer indices" [0 1 2]
WorldEnd
`);
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  const file = resolve(root, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname));
  if (!file.startsWith(root + sep)) { response.writeHead(403).end(); return; }
  try {
    const bytes = await readFile(file);
    const type = file.endsWith('.wasm') ? 'application/wasm' : file.endsWith('.js') ? 'text/javascript'
      : file.endsWith('.css') ? 'text/css' : file.endsWith('.png') ? 'image/png' : 'text/html';
    response.writeHead(200, {'Content-Type': type, 'Cache-Control': 'no-store'}).end(bytes);
  } catch { response.writeHead(404).end(); }
});
await new Promise(done => server.listen(0, '127.0.0.1', done));
const browser = await chromium.launch({executablePath: process.env.LUDUS_QA_BROWSER || undefined, args: launchArgs});
report.browser = browser.version();
const base = `http://127.0.0.1:${server.address().port}`;
const canvas = page => page.locator('.ludus-gpu-viewport').last();
const frames = page => canvas(page).getAttribute('data-frames').then(Number);
async function click(page, x, y) {
  await page.mouse.click(x, y);
  // Qt consumes input and relayouts its canvas on the event-loop/animation turn.
  await page.waitForTimeout(200);
}
async function open(page) {
  page.on('pageerror', error => report.errors.push(error.message));
  await page.goto(base, {waitUntil: 'domcontentloaded'});
  await page.waitForFunction(() => document.body.dataset.editorState === 'ready', null, {timeout: 60000});
  // Fixed desktop acceptance geometry. Qt controls are canvas-drawn, without DOM locators.
  await click(page, 273, 191); // World tab.
  await click(page, 78, 279); // New World.
  await click(page, 680, 666); // Save dialog filename.
  await page.keyboard.type('gpu.world.json');
  await click(page, 991, 664);
}
async function importScene(page) {
  await click(page, 551, 280);
  await page.screenshot({path: resolve(output, 'import-menu.png')});
  const chooser = page.waitForEvent('filechooser', {timeout: 10000});
  await click(page, 556, 334);
  await (await chooser).setFiles({name: 'scene.pbrt', mimeType: 'text/plain', buffer: source});
  await page.waitForTimeout(500);
  await click(page, 1010, 317); // Import previewed scene: actual SceneCook + MEMFS publication.
}
async function rendered(page, previous = 0, isRecovering = false) {
  await page.waitForFunction(({count, isRecovering}) => {
    const surface = [...document.querySelectorAll('.ludus-gpu-viewport')].at(-1);
    return (!isRecovering && document.querySelector('#screen').dataset.viewportError) ||
      (surface?.style.display === 'block' && Number(surface.dataset.frames) > count + 2);
  }, {count: previous, isRecovering}, {timeout: 30000});
  report.gpuDiagnostics = await page.evaluate(() => globalThis.qaGpuErrors || []);
  assert.equal(await page.locator('#screen').getAttribute('data-viewport-error'), '');
  await page.evaluate(() => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done))));
}
async function shot(page, name) {
  const box = await canvas(page).boundingBox();
  assert.ok(box && box.width > 1 && box.height > 1);
  const bytes = await page.screenshot({clip: box});
  await writeFile(resolve(output, name + '.png'), bytes);
  const image = PNG.sync.read(bytes);
  let lit = 0, warm = 0;
  for (let offset = 0; offset < image.data.length; offset += 4) {
    const [red, green, blue] = image.data.subarray(offset, offset + 3);
    if (Math.max(red, green, blue) > 32) ++lit;
    if (red > green + 30 && red > blue + 30) ++warm;
  }
  const center = (Math.floor(image.height / 2) * image.width + Math.floor(image.width / 2)) * 4;
  return {bytes, lit, warm, center: [...image.data.subarray(center, center + 3)],
    width: image.width, height: image.height};
}
let activePage;
try {
  const page = await browser.newPage({viewport: {width: 1440, height: 1000}});
  activePage = page;
  await page.addInitScript(() => {
    globalThis.qaGpuErrors = [];
    const create = GPUDevice.prototype.createRenderPipelineAsync;
    GPUDevice.prototype.createRenderPipelineAsync = function (...parameters) {
      if (globalThis.qaRejectNextPipeline) {
        globalThis.qaRejectNextPipeline = false;
        return Promise.reject(new GPUPipelineError('QA rejected candidate pipeline', {reason: 'validation'}));
      }
      const begin = () => create.apply(this, parameters).catch(error => {
        globalThis.qaGpuErrors.push(error.message); throw error;
      });
      if (globalThis.qaDelayNextPipeline) {
        globalThis.qaDelayNextPipeline = false;
        return new Promise(done => setTimeout(done, 500)).then(begin);
      }
      return begin();
    };
    const pop = GPUDevice.prototype.popErrorScope;
    GPUDevice.prototype.popErrorScope = function () {
      return pop.call(this).then(error => { if (error) globalThis.qaGpuErrors.push(error.message); return error; });
    };
    const request = GPUAdapter.prototype.requestDevice;
    GPUAdapter.prototype.requestDevice = function (...parameters) {
      const info = this.info;
      globalThis.qaViewportAdapter = {vendor: info.vendor, architecture: info.architecture,
        device: info.device, description: info.description};
      return request.apply(this, parameters).then(device => { globalThis.qaViewportDevice = device; return device; });
    };
  });
  await open(page);
  await page.screenshot({path: resolve(output, 'world-opened.png')});
  await importScene(page);
  await rendered(page);
  report.adapter = await page.evaluate(() => globalThis.qaViewportAdapter);
  const first = await shot(page, 'solid');
  assert.ok(first.lit > 100, 'Imported geometry must be visible in composited GPU pixels');
  report.cases.push('PBRT import, immutable MEMFS publication and visible GPU Solid pixels');
  const beforeZoom = await frames(page);
  const box = await canvas(page).boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.wheel(0, -160);
  await rendered(page, beforeZoom);
  const zoomed = await shot(page, 'zoom');
  assert.notDeepEqual(zoomed.bytes, first.bytes, 'Navigation must change actual GPU pixels');
  const beforeOrbit = await frames(page);
  await page.keyboard.down('Alt');
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 + 45, box.y + box.height / 2 + 10, {steps: 5});
  await page.mouse.up();
  await page.keyboard.up('Alt');
  await rendered(page, beforeOrbit);
  assert.notDeepEqual((await shot(page, 'orbit')).bytes, zoomed.bytes);
  report.cases.push('Qt wheel and orbit input reaches the GPU camera');
  for (const mode of ['albedo', 'lit']) {
    const previous = await frames(page);
    const rectangle = await canvas(page).boundingBox();
    await click(page, rectangle.x + 100, rectangle.y - 48);
    await page.screenshot({path: resolve(output, mode + '-menu.png')});
    await click(page, rectangle.x + 100, rectangle.y - 17);
    await page.screenshot({path: resolve(output, mode + '-selected.png')});
    await rendered(page, previous);
    assert.ok((await shot(page, mode)).warm > 50, 'Authored material color must reach the GPU image');
  }
  report.cases.push('Albedo and authored Lit material replacement with directional shadow preparation');
  const accepted = await shot(page, 'accepted-lit');
  const toolbar = await canvas(page).boundingBox();
  const previous = await frames(page);
  await page.evaluate(() => { globalThis.qaDelayNextPipeline = true; });
  await click(page, toolbar.x + 100, toolbar.y - 48);
  await click(page, toolbar.x + 100, toolbar.y - 98); // Lit -> Solid.
  assert.deepEqual((await shot(page, 'pending')).bytes, accepted.bytes,
    'Pending preparation must retain the last GPU image');
  await rendered(page, previous);
  const solid = await shot(page, 'published-solid');
  assert.ok(solid.lit > 100 && solid.warm === 0);
  await page.evaluate(() => { globalThis.qaRejectNextPipeline = true; });
  await click(page, toolbar.x + 100, toolbar.y - 48);
  await click(page, toolbar.x + 100, toolbar.y - 17); // Solid -> Albedo.
  await page.waitForFunction(() => document.querySelector('#screen').dataset.viewportError, null, {timeout: 10000});
  const retained = await shot(page, 'rejected');
  assert.ok(retained.lit > 50);
  assert.ok(retained.center.every((value, index) => Math.abs(value - solid.center[index]) <= 2),
    'A rejected candidate must retain the previous GPU image');
  const beforeRecovery = await frames(page);
  await page.evaluate(() => { globalThis.qaGpuErrors = []; });
  await click(page, toolbar.x + 100, toolbar.y - 48);
  await click(page, toolbar.x + 100, toolbar.y - 17); // Requested Albedo -> Lit.
  await rendered(page, beforeRecovery, true);
  assert.ok((await shot(page, 'recovered-lit')).warm > 50);
  report.cases.push('Synthetic delayed/rejected preparation retains prior pixels, then successfully publishes recovery');
  await click(page, 45, 191);
  await page.waitForTimeout(200);
  assert.equal(await canvas(page).evaluate(node => node.style.display), 'none');
  const hidden = await frames(page);
  await page.waitForTimeout(250);
  assert.equal(await frames(page), hidden, 'Hidden viewports must stop repeated GPU submissions');
  await click(page, 273, 191);
  await rendered(page, hidden);
  const beforeResize = await frames(page);
  await page.setViewportSize({width: 1200, height: 900});
  await rendered(page, beforeResize);
  const resized = await shot(page, 'resized');
  assert.notEqual(resized.width, first.width);
  assert.ok(resized.lit > 50);
  report.cases.push('Tab visibility, resumed presentation and resized GPU pixels');
  await page.evaluate(() => globalThis.qaViewportDevice.destroy());
  await page.waitForFunction(() => document.querySelector('#screen').dataset.viewportError, null, {timeout: 10000});
  assert.equal(await page.locator('body').getAttribute('data-editor-state'), 'ready');
  report.cases.push('Actual GPU device destruction reports a viewport failure while editor stays available');
  await page.close();
  const unavailable = await browser.newPage({viewport: {width: 1440, height: 1000}});
  activePage = unavailable;
  await unavailable.addInitScript(() => Object.defineProperty(navigator, 'gpu', {value: undefined}));
  await open(unavailable);
  await importScene(unavailable);
  await unavailable.waitForFunction(() => document.querySelector('#screen').dataset.viewportError, null, {timeout: 10000});
  assert.match(await unavailable.locator('#screen').getAttribute('data-viewport-error'), /WebGPU/);
  assert.equal(await unavailable.locator('body').getAttribute('data-editor-state'), 'ready');
  await unavailable.screenshot({path: resolve(output, 'unavailable.png')});
  report.cases.push('Missing WebGPU reports failure without a CPU rendering fallback');
  assert.deepEqual(report.errors, []);
  report.passed = true;
} catch (error) {
  report.failure = String(error);
  if (activePage && !activePage.isClosed())
    await activePage.screenshot({path: resolve(output, 'failure.png')});
  throw error;
} finally {
  await writeFile(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  await browser.close();
  await new Promise(done => server.close(done));
}
console.log(JSON.stringify(report, null, 2));
