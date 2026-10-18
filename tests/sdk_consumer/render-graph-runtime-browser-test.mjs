// Installed-SDK RG3 runtime generations on WebGPU. render_graph_runtime.cpp renders
// two cooked offscreen graphs (opaque static, and masked with per-draw uniform values)
// from their compiled headers, then decodes each graph's WGSL `.lurgrt` generation,
// activates it twice through RenderGraphActivation and compares every exported RGBA8
// byte, read back through the RHI, inside Wasm.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, access} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/render-graph-runtime-browser');
// render_graph_runtime.cpp activates the generation this many times; a shorter run cannot pass.
const GENERATION_COUNT = 2;
// The opaque graph, then the masked graph with per-draw uniforms; the run ends on the last one.
const LAST_FIXTURE = 1;
await mkdir(output, {recursive:true});
for (const name of ['render-graph-runtime.html','render-graph-runtime.js','render-graph-runtime.wasm']) await access(resolve(root,name));
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
const report = {kind:'real Chromium WebGPU on SwiftShader; physical GPU acceptance separate', args, cases:[], console:[]};
let browser, page;
try {
  browser = await chromium.launch({headless:true, args});
  const pin = JSON.parse(await readFile(new URL('../../config/shader_toolchain.json',import.meta.url))).wgsl_validator;
  assert.equal(browser.version(),pin.chromium_version);
  report.version = browser.version();
  const context = await browser.newContext({viewport:{width:200,height:160},deviceScaleFactor:1});
  page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => {
    report.console.push({scenario:'render-graph-runtime-webgpu',type:message.type(),text:message.text()});
    if (message.type() === 'error') errors.push(message.text());
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/render-graph-runtime.html?backend=webgpu`);
  await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:60000});
  const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
  assert.equal(status.state,'passed',JSON.stringify({status,errors}));
  assert.equal(status.backend,'webgpu',JSON.stringify(status));
  assert.equal(Number(status.generation),GENERATION_COUNT,JSON.stringify(status));
  assert.equal(Number(status.fixture),LAST_FIXTURE,JSON.stringify(status));
  assert.equal(status.mismatches,'0',JSON.stringify(status));
  assert.notEqual(status.center,'0','the compiled graph must draw the triangle');
  assert.deepEqual(errors,[]);
  report.cases.push({scenario:'render-graph-runtime-webgpu',outcome:'passed',fixtures:LAST_FIXTURE + 1,generations:GENERATION_COUNT,mismatches:0,center:Number(status.center)});
  await context.close();
  report.outcome = 'passed';
} catch (error) {
  report.outcome='failed'; report.error=String(error);
  report.lastStatus = page ? await page.locator('#status').evaluate(node => ({text:node.textContent,...node.dataset})).catch(() => null) : null;
  // The page reports its step, capture sub-step and last RasterStatus; show them beside a timeout.
  console.error('Last fixture status:', JSON.stringify(report.lastStatus));
  throw error;
} finally {
  await writeFile(resolve(output,'report.json'),JSON.stringify(report,null,2)+'\n');
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
