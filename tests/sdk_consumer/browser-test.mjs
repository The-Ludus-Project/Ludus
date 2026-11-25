// Real browser checks of the installed-SDK fullscreen diagnostic (render.cpp drawing
// shaders/diagnostic.lusl) on forced WebGPU, forced WebGL 2, Auto, and Auto with WebGPU
// unavailable. Injected failures wrap real WebGPU objects. This harness is a development tool
// and is never packaged. The raster browser suite (browser-suites.json) runs it on the batch's
// raster-web-sdk-consumer artifact.
//
// Usage: node browser-test.mjs [consumer directory] [results directory]
// The consumer directory holds render.html, render.js and render.wasm. Chromium uses the
// SwiftShader software adapter; LUDUS_BROWSER_DEFAULT_ADAPTER=1 runs flag-free instead, to record
// what the default adapter does.
import assert from 'node:assert/strict';
import {access, readFile, writeFile, mkdir} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const {PNG} = require('pngjs');
const root = resolve(process.argv[2] || 'out/build/render-api-sdk-web');
const output = resolve(process.argv[3] || 'out/render-api-browser');
const pin = JSON.parse(await readFile(new URL('../../config/shader_toolchain.json', import.meta.url))).wgsl_validator;
await mkdir(output, {recursive:true});
for (const name of ['render.html', 'render.js', 'render.wasm']) await access(resolve(root, name));
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  const path = resolve(root, '.' + pathname);
  if (!path.startsWith(root + sep)) {response.writeHead(403).end(); return;}
  try {response.writeHead(200, {'Content-Type':path.endsWith('.wasm') ? 'application/wasm' : path.endsWith('.js') ? 'text/javascript' : 'text/html'}).end(await readFile(path));}
  catch {response.writeHead(404).end();}
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const isDefaultAdapter = process.env.LUDUS_BROWSER_DEFAULT_ADAPTER === '1';
const args = isDefaultAdapter ? [] : ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--use-vulkan=swiftshader', '--disable-vulkan-surface', '--enable-unsafe-swiftshader'];
const report = {kind:isDefaultAdapter ? 'default browser adapter' : 'real Chromium software GPU (SwiftShader); physical GPU acceptance separate', args, cases:[], console:[]};
// Pixel cases run on every API axis. 'fallback' is Auto with navigator.gpu removed, so the one
// WebGL 2 attempt must render. The failure cases inject WebGPU faults and force WebGPU.
const cases = [
  {scenario:'success', selection:'webgpu', expectedBackend:'webgpu'},
  {scenario:'success', selection:'webgl2', expectedBackend:'webgl2'},
  {scenario:'success', selection:'auto', expectedBackend:'webgpu'},
  {scenario:'success', selection:'fallback', expectedBackend:'webgl2'},
  {scenario:'cancel-pending', selection:'webgpu', expectedBackend:'webgpu'},
  {scenario:'invalid-shader', selection:'webgpu'},
  {scenario:'invalid-pipeline', selection:'webgpu'},
  {scenario:'device-loss', selection:'webgpu'},
];
// Frames at which the page pauses for a screenshot; the consumer stops after 32 frames.
const CAPTURED_FRAMES = [1, 5, 9, 13, 17, 21, 25, 29];
const MAXIMUM_CHANNEL_ERROR = 2;
let browser; let lastPage;
try {
  browser = await chromium.launch({headless:true, args});
  assert.equal(browser.version(), pin.chromium_version);
  report.version = browser.version();
  for (const {scenario, selection, expectedBackend} of cases) {
    const name = `${scenario}-${selection}`;
    const context = await browser.newContext({viewport:{width:200,height:200}, deviceScaleFactor:1});
    await context.addInitScript(({scenario, selection, capturedFrames}) => {
      if (selection === 'fallback') Object.defineProperty(navigator, 'gpu', {get:() => undefined});
      window.__qaPause = false;
      window.__qaStats = {pipelines:0,buffers:0,submits:0,writes:0,devices:0};
      window.Module = {arguments:scenario === 'cancel-pending' ? ['cancel-pending'] : []};
      new MutationObserver(() => {
        const frames = Number(document.querySelector('#status')?.dataset.frames);
        if (capturedFrames.includes(frames)) window.__qaPause = true;
      }).observe(document, {attributes:true,subtree:true});
      if (!navigator.gpu) return;
      const requestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
      navigator.gpu.requestAdapter = async (...args) => {
        const adapter = await requestAdapter(...args);
        if (!adapter) return adapter;
        const requestDevice = adapter.requestDevice.bind(adapter);
        adapter.requestDevice = async (...args) => {
          const device = await requestDevice(...args); ++window.__qaStats.devices;
          window.__qaDevice = device;
          for (const [name, counter] of [['createRenderPipeline','pipelines'], ['createBuffer','buffers']]) {
            const create = device[name].bind(device);
            device[name] = desc => {++window.__qaStats[counter]; return create(name === 'createRenderPipeline' && scenario === 'invalid-pipeline' ? {...desc, vertex:{...desc.vertex, entryPoint:'missing'}} : desc);};
          }
          const shader = device.createShaderModule.bind(device);
          device.createShaderModule = desc => shader(scenario === 'invalid-shader' ? {...desc, code:'invalid WGSL'} : desc);
          const scope = device.popErrorScope.bind(device);
          device.popErrorScope = () => {
            const promise = scope();
            return scenario === 'cancel-pending' ? promise.then(result => new Promise(resolve => setTimeout(() => resolve(result), 100))) : promise;
          };
          for (const [name,counter] of [['submit','submits'],['writeBuffer','writes']]) {
            const invoke = device.queue[name].bind(device.queue);
            device.queue[name] = (...args) => {++window.__qaStats[counter]; return invoke(...args);};
          }
          return device;
        };
        return adapter;
      };
    }, {scenario, selection, capturedFrames:CAPTURED_FRAMES});
    const page = await context.newPage();
    lastPage = page;
    page.on('console', message => report.console.push({scenario:name,type:message.type(),text:message.text()}));
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.address().port}/render.html?backend=${selection === 'fallback' ? 'auto' : selection}`);
    const result = {scenario, selection, pixels:[]};
    if (scenario === 'success' || scenario === 'cancel-pending') {
      for (const frame of CAPTURED_FRAMES) {
        await page.waitForFunction(frame => document.querySelector('#status').dataset.frames === String(frame), frame, {timeout:15000,polling:10});
        result.backend = await page.locator('#status').getAttribute('data-backend');
        assert.equal(result.backend, expectedBackend, `${name}: selected backend`);
        const bytes = await page.locator('#canvas').screenshot();
        const image = PNG.sync.read(bytes); const elapsed = (frame - 1) % 8 < 4 ? 0 : 2;
        // diagnostic.lusl with Tint (0.6, 0.4, 0.2, 1) and Direction (0.2, 0.4, 0.6): red follows
        // u, green follows v from the top-left origin, blue marks the centred circle, and the
        // elapsed term shifts every channel.
        let maximum = 0;
        const rowGreen = [0, 0];
        for (let y = 0; y < image.height; ++y) for (let x = 0; x < image.width; ++x) {
          const cx = (x + .5 - image.width / 2) / image.height, cy = (y + .5 - image.height / 2) / image.height;
          const expected = [.6*(x+.5)/image.width+.01*elapsed, .4*(y+.5)/image.height+.02*elapsed, .2*(cx*cx+cy*cy<.04?1:0)+.03*elapsed, 1];
          for (let c = 0; c < 4; ++c) maximum = Math.max(maximum, Math.abs(image.data[(y*image.width+x)*4+c] - Math.round(expected[c]*255)));
          if (y === 0) rowGreen[0] += image.data[(y*image.width+x)*4+1];
          if (y === image.height - 1) rowGreen[1] += image.data[(y*image.width+x)*4+1];
        }
        // A Y mirror (a missing or doubled LUDUS_GLSL_ES flip) makes the top row the greener one.
        assert(rowGreen[0] < rowGreen[1], `${name} frame ${frame}: top row green ${rowGreen[0]} is not below bottom row ${rowGreen[1]}`);
        assert(maximum <= MAXIMUM_CHANNEL_ERROR, `${name} frame ${frame}: maximum error ${maximum}`);
        result.pixels.push({frame,width:image.width,height:image.height,elapsed,maximum,topRowGreen:rowGreen[0],bottomRowGreen:rowGreen[1]});
        await writeFile(resolve(output, `${name}-${frame}.png`), bytes);
        await page.evaluate(() => {window.__qaPause=false;});
      }
      await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'passed',null,{polling:10});
      assert.equal(await page.locator('#status').getAttribute('data-frames'), '32');
      result.stats = await page.evaluate(() => window.__qaStats);
      if (expectedBackend === 'webgpu') {
        assert.equal(result.stats.pipelines, 2); assert.equal(result.stats.buffers, scenario === 'cancel-pending' ? 3 : 2);
        assert.equal(result.stats.submits,32); assert.equal(result.stats.writes,32);
        assert.equal(result.stats.devices,scenario === 'cancel-pending' ? 3 : 2);
      } else {
        // Forced WebGL 2 never requests a WebGPU device; the fallback case has no navigator.gpu.
        assert.equal(result.stats.devices, 0);
      }
    } else {
      if (scenario === 'device-loss') {
        await page.waitForFunction(() => document.querySelector('#status').dataset.frames === '1',null,{polling:10});
        await page.evaluate(() => {window.__qaDevice.destroy(); window.__qaPause=false;});
      }
      await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'failed', null, {timeout:15000,polling:10});
      result.stats = await page.evaluate(() => window.__qaStats);
      if (scenario !== 'device-loss') assert.equal(result.stats.submits,0);
    }
    assert.deepEqual(errors,[]); result.outcome = 'passed'; report.cases.push(result);
    await context.close();
  }
  report.outcome='passed';
} catch (error) {report.outcome='failed'; report.error=String(error); report.lastStatus = lastPage ? await lastPage.locator('#status').evaluate(element => ({text:element.textContent,...element.dataset})) : null; throw error;}
finally {await writeFile(resolve(output,'report.json'), JSON.stringify(report,null,2)+'\n'); await browser?.close(); await new Promise(resolve => server.close(resolve));}
