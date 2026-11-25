// Installed-SDK R12a indirect draws and dispatches in Chromium. indirect.cpp writes
// offset, strided, zero-instance and non-indexed commands plus two dispatch records on
// the GPU and compares the image with the direct-draw oracle; WebGL 2 must report an
// empty profile, refuse the argument buffer and draw the same image through the
// authored direct variant. See
// docs/development/fullscreen-rendering.md#expanded-indirect-draws-and-dispatches-r12a.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, access} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/indirect-browser');
// Values shared with indirect.cpp and the public RHI enumerations.
const FEATURE = 'indirect-first-instance';
const BACKEND = {webgpu:1, webgl2:2};
const PATH = {indirect:1, zeroFirstInstance:2, direct:3};
const UNSUPPORTED = 7;
const MARKS = ['100','101','102','0'];
await mkdir(output, {recursive:true});
for (const name of ['indirect.html','indirect.js','indirect.wasm']) await access(resolve(root,name));
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  const path = resolve(root, '.' + pathname);
  if (!path.startsWith(root + sep)) {response.writeHead(403).end(); return;}
  try {
    const bytes = await readFile(path);
    response.writeHead(200, {'Content-Type':path.endsWith('.wasm') ? 'application/wasm' : path.endsWith('.js') ? 'text/javascript' : 'text/html'}).end(bytes);
  }
  catch {response.writeHead(404).end();}
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const args = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--use-vulkan=swiftshader', '--disable-vulkan-surface', '--enable-unsafe-swiftshader'];
const report = {kind:'real Chromium software GPU; physical GPU acceptance separate', args, cases:[], console:[]};
const scenarios = [
  {name:'webgpu', backend:'webgpu'},
  {name:'webgpu-no-first-instance', backend:'webgpu', hideFeature:true},
  {name:'webgl2', backend:'webgl2'},
  {name:'auto-fallback', backend:'auto', hideWebGpu:true},
];
let browser, page, current;
try {
  browser = await chromium.launch({headless:true, args});
  const pin = JSON.parse(await readFile(new URL('../../config/shader_toolchain.json',import.meta.url))).wgsl_validator;
  assert.equal(browser.version(),pin.chromium_version);
  report.version = browser.version();
  for (const scenario of scenarios) {
    current = scenario.name;
    const context = await browser.newContext();
    // Record what the adapter offered and what the engine requested; optionally hide the
    // optional feature (as an adapter without it would) or WebGPU itself.
    await context.addInitScript(({hideFeature, hideWebGpu, feature}) => {
      window.__qaAdapter = null;
      window.__qaRequiredFeatures = null;
      if (hideWebGpu) Object.defineProperty(navigator,'gpu',{get:() => undefined});
      if (!navigator.gpu) return;
      const requestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
      navigator.gpu.requestAdapter = async (...args) => {
        const adapter = await requestAdapter(...args);
        if (!adapter) return adapter;
        window.__qaAdapter = {hasFeature:adapter.features.has(feature), ...(adapter.info ? {vendor:adapter.info.vendor,architecture:adapter.info.architecture,description:adapter.info.description} : {})};
        if (hideFeature) Object.defineProperty(adapter,'features',{value:new Set([...adapter.features].filter(name => name !== feature))});
        const requestDevice = adapter.requestDevice.bind(adapter);
        adapter.requestDevice = async (descriptor = {}) => {
          window.__qaRequiredFeatures = [...(descriptor.requiredFeatures || [])];
          return requestDevice(descriptor);
        };
        return adapter;
      };
    },{hideFeature:scenario.hideFeature === true, hideWebGpu:scenario.hideWebGpu === true, feature:FEATURE});
    page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => report.console.push({scenario:scenario.name,type:message.type(),text:message.text()}));
    await page.goto(`http://127.0.0.1:${server.address().port}/indirect.html?backend=${scenario.backend}`);
    await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:60000});
    const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
    const adapter = await page.evaluate(() => window.__qaAdapter);
    const requiredFeatures = await page.evaluate(() => window.__qaRequiredFeatures);
    const detail = JSON.stringify({status,adapter,requiredFeatures});
    assert.equal(status.state,'passed',detail);
    assert.equal(status.mismatches,'0',detail);
    assert.deepEqual(errors,[]);
    const result = {scenario:scenario.name,outcome:'passed',status,adapter,requiredFeatures};
    if (scenario.backend === 'webgpu') {
      // Every R12a operation except FirstInstance is core WebGPU; the profile is fixed.
      assert.equal(Number(status.backend),BACKEND.webgpu,detail);
      assert.equal(status.indirectRendering,'1',detail);
      assert.equal(status.unmetIndirect,'0',detail);
      assert.equal(status.alignment,'4',detail);
      assert.equal(status.maxCommands,'64',detail);
      assert.equal(status.nonIndexed,'1',detail);
      assert.equal(status.offsets,'1',detail);
      assert.equal(status.dispatch,'1',detail);
      assert.equal(status.computePasses,'3',detail);
      assert.equal(status.rasterPasses,'1',detail);
      assert.equal(status.draws,'2',detail);
      assert.deepEqual([0,1,2,3].map(i => status[`mark${i}`]),MARKS,detail);
      assert.deepEqual([0,1,2,3].map(i => status[`emptyMark${i}`]),['0','0','0','0'],detail);
      assert.ok(adapter !== null,'the fixture must request a WebGPU adapter');
      // The device requests the feature exactly when the adapter it sees exposes it.
      const isVisible = adapter.hasFeature === true && scenario.hideFeature !== true;
      assert.equal(requiredFeatures.includes(FEATURE),isVisible,detail);
      assert.equal(status.firstInstance,isVisible ? '1' : '0',detail);
      assert.equal(Number(status.path),isVisible ? PATH.indirect : PATH.zeroFirstInstance,detail);
      result.firstInstanceExercised = isVisible;
    } else {
      // WebGL 2: empty profile, refused argument buffer and the authored direct variant.
      assert.equal(Number(status.backend),BACKEND.webgl2,detail);
      assert.equal(Number(status.path),PATH.direct,detail);
      assert.equal(status.indirectRendering,'0',detail);
      assert.equal(status.unmetIndirect,'1',detail);
      for (const key of ['alignment','maxCommands','nonIndexed','offsets','firstInstance','dispatch']) assert.equal(status[key],'0',`${key} ${detail}`);
      assert.equal(Number(status.storageIndirect),UNSUPPORTED,detail);
      assert.equal(status.computePasses,'0',detail);
      assert.equal(status.rasterPasses,'1',detail);
      assert.equal(status.draws,'2',detail);
      assert.equal(status.requiredRejected,scenario.backend === 'webgl2' ? '1' : '0',detail);
      if (scenario.hideWebGpu) assert.equal(adapter,null,detail);
    }
    report.cases.push(result);
    await context.close();
  }
  // Without the optional feature on this adapter, nonzero FirstInstance stays native-only evidence.
  report.firstInstanceExercised = report.cases.some(item => item.firstInstanceExercised === true);
  report.outcome = 'passed';
} catch (error) {
  report.outcome='failed'; report.error=String(error); report.scenario=current;
  report.lastStatus = page ? await page.locator('#status').evaluate(node => ({text:node.textContent,...node.dataset})).catch(() => null) : null;
  throw error;
} finally {
  await writeFile(resolve(output,'report.json'),JSON.stringify(report,null,2)+'\n');
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
