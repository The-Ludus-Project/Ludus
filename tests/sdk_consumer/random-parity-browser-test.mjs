// Installed-SDK addressed-random parity on WebGPU. random_parity.cpp runs the
// corpus shared with the native renderer test through the browser compute path
// and compares every word with the CPU reference compiled to WebAssembly.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, access} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/random-parity-browser');
// Cases in random_parity_corpus.h after padding to whole 64-invocation groups;
// update together with the corpus so a truncated run cannot pass.
const EXPECTED_CASES = 172928;
await mkdir(output, {recursive:true});
for (const name of ['random_parity.html','random_parity.js','random_parity.wasm']) await access(resolve(root,name));
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
  const context = await browser.newContext();
  // The fixture must reach WebGPU, never a fallback; record the adapter it used.
  await context.addInitScript(() => {
    window.__qaAdapters = [];
    if (navigator.gpu) {
      const requestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
      navigator.gpu.requestAdapter = async (...args) => {
        const adapter = await requestAdapter(...args);
        if (adapter) window.__qaAdapters.push({...(adapter.info ? {vendor:adapter.info.vendor,architecture:adapter.info.architecture,description:adapter.info.description} : {})});
        return adapter;
      };
    }
  });
  page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => report.console.push({scenario:'random-parity-webgpu',type:message.type(),text:message.text()}));
  await page.goto(`http://127.0.0.1:${server.address().port}/random_parity.html`);
  await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:120000});
  const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
  const adapters = await page.evaluate(() => window.__qaAdapters);
  assert.equal(status.state,'passed',JSON.stringify(status));
  assert.equal(Number(status.cases),EXPECTED_CASES,JSON.stringify(status));
  assert.equal(status.mismatches,'0',JSON.stringify(status));
  assert.ok(adapters.length >= 1,'the fixture must request a WebGPU adapter');
  assert.ok(report.console.some(message => message.text.includes(`Random parity passed true`) && message.text.includes(`cases ${EXPECTED_CASES}; mismatches 0`)));
  assert.deepEqual(errors,[]);
  report.cases.push({scenario:'random-parity-webgpu',outcome:'passed',cases:Number(status.cases),mismatches:0,adapters});
  await context.close();
  report.outcome = 'passed';
} catch (error) {
  report.outcome='failed'; report.error=String(error);
  report.lastStatus = page ? await page.locator('#status').evaluate(node => ({text:node.textContent,...node.dataset})).catch(() => null) : null;
  throw error;
} finally {
  await writeFile(resolve(output,'report.json'),JSON.stringify(report,null,2)+'\n');
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
