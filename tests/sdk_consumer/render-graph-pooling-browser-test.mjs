// Installed-SDK RG4 exact-descriptor transient pooling evidence on WebGPU. render_graph_pooling.cpp
// runs the export-only and scratch-2 workloads with the serial control and the pooled path, compares
// every captured exported byte inside Wasm and logs one `RG4 run` line per run. This runner records
// those measurements with browser and timer metadata in report.json and requires a passing run.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, access} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/render-graph-pooling-browser');
// render_graph_pooling.cpp: two workloads, each with the serial control and the pooled path.
const RUN_COUNT = 4;
await mkdir(output, {recursive:true});
for (const name of ['render-graph-pooling.html','render-graph-pooling.js','render-graph-pooling.wasm']) await access(resolve(root,name));
// Cross-origin isolation gives performance.now() Chromium's finer isolated-context resolution.
const isolation = {'Cross-Origin-Opener-Policy':'same-origin', 'Cross-Origin-Embedder-Policy':'require-corp'};
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  const path = resolve(root, '.' + pathname);
  if (!path.startsWith(root + sep)) {response.writeHead(403).end(); return;}
  try {
    const bytes = await readFile(path);
    const type = path.endsWith('.wasm') ? 'application/wasm' : path.endsWith('.js') ? 'text/javascript' : 'text/html';
    response.writeHead(200, {'Content-Type':type, ...isolation}).end(bytes);
  }
  catch {response.writeHead(404).end();}
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const args = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--use-vulkan=swiftshader', '--disable-vulkan-surface', '--enable-unsafe-swiftshader'];
const report = {kind:'real Chromium WebGPU on SwiftShader; physical GPU acceptance separate', args, runs:[], console:[]};
/// Parse `key=value` fields; a quoted value may contain spaces.
const parseFields = text => Object.fromEntries([...text.matchAll(/(\w+)=("[^"]*"|\S+)/g)].map(([, key, value]) =>
  [key, value.startsWith('"') ? value.slice(1, -1) : Number.isNaN(Number(value)) ? value : Number(value)]));
let browser, page;
try {
  browser = await chromium.launch({headless:true, args});
  const pin = JSON.parse(await readFile(new URL('../../config/shader_toolchain.json',import.meta.url))).wgsl_validator;
  assert.equal(browser.version(),pin.chromium_version);
  report.version = browser.version();
  const context = await browser.newContext({viewport:{width:640,height:400},deviceScaleFactor:1});
  page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => {
    const text = message.text();
    report.console.push({type:message.type(), text});
    if (message.type() === 'error') errors.push(text);
    if (text.includes('device backend=')) report.device = parseFields(text.slice(text.indexOf('device backend=')));
    if (text.includes('run workload=')) report.runs.push(parseFields(text.slice(text.indexOf('run workload='))));
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/render-graph-pooling.html`);
  report.timer = await page.evaluate(() => {
    // Smallest nonzero step observed between performance.now() readings.
    let smallest = Infinity;
    let last = performance.now();
    for (let i = 0; i < 200000; ++i) {
      const now = performance.now();
      if (now > last) {smallest = Math.min(smallest, now - last); last = now;}
    }
    return {crossOriginIsolated, smallestStepMicroseconds:smallest * 1000};
  });
  await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:240000});
  const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
  assert.equal(status.state,'passed',JSON.stringify({status,errors}));
  assert.equal(status.mismatches,'0',JSON.stringify(status));
  assert.equal(report.runs.length,RUN_COUNT,JSON.stringify(report.runs));
  for (const run of report.runs) assert.equal(run.mismatched_bytes,0,JSON.stringify(run));
  assert.deepEqual(errors,[]);
  await context.close();
  report.outcome = 'passed';
} catch (error) {
  report.outcome='failed'; report.error=String(error);
  report.lastStatus = page ? await page.locator('#status').evaluate(node => ({text:node.textContent,...node.dataset})).catch(() => null) : null;
  console.error('Last pooling status:', JSON.stringify(report.lastStatus));
  throw error;
} finally {
  await writeFile(resolve(output,'report.json'),JSON.stringify(report,null,2)+'\n');
  for (const run of report.runs) console.log(JSON.stringify(run));
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
