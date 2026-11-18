// Installed-SDK managed bilinear upscale pipelines on WebGPU. render_graph_bilinear.cpp draws a
// half-extent scene pass upscaled bilinearly into an exported RGBA8 target three times: without a
// program cache, then as an active and a candidate graph sharing one RenderGraphProgramCache. The
// candidate must join the active graph's native pipeline, and every exported byte of both shared
// graphs, read back through the RHI inside Wasm, must equal the unshared graph's bytes.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, access} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/render-graph-bilinear-browser');
// Unshared, active and candidate graphs, in that order; the run ends on the candidate.
const GRAPH_NAMES = ['unshared', 'active', 'candidate'];
await mkdir(output, {recursive:true});
for (const name of ['render-graph-bilinear.html','render-graph-bilinear.js','render-graph-bilinear.wasm']) await access(resolve(root,name));
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
const report = {kind:'real Chromium WebGPU on SwiftShader; physical GPU acceptance separate', args, graphs:[], console:[]};
/// Parse `key=value` fields of one consumer log line.
const parseFields = text => Object.fromEntries([...text.matchAll(/(\w+)=(\S+)/g)].map(([, key, value]) =>
  [key, Number.isNaN(Number(value)) ? value : Number(value)]));
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
    const text = message.text();
    report.console.push({type:message.type(), text});
    if (message.type() === 'error') errors.push(text);
    if (text.includes('bilinear graph=')) report.graphs.push(parseFields(text.slice(text.indexOf('bilinear graph='))));
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/render-graph-bilinear.html`);
  await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:60000});
  const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
  assert.equal(status.state,'passed',JSON.stringify({status,errors}));
  assert.equal(status.backend,'webgpu',JSON.stringify(status));
  assert.equal(Number(status.graph),GRAPH_NAMES.length - 1,JSON.stringify(status));
  assert.equal(status.mismatches,'0',JSON.stringify(status));
  assert.equal(status.shared,'1',JSON.stringify(status));
  assert.notEqual(status.center,'0','the upscaled graph must draw the triangle');
  assert.deepEqual(report.graphs.map(graph => graph.graph),GRAPH_NAMES,JSON.stringify(report.graphs));
  assert.deepEqual(report.graphs.map(graph => graph.shared_requests),[0,0,1],JSON.stringify(report.graphs));
  assert.deepEqual(errors,[]);
  await context.close();
  report.outcome = 'passed';
} catch (error) {
  report.outcome='failed'; report.error=String(error);
  report.lastStatus = page ? await page.locator('#status').evaluate(node => ({text:node.textContent,...node.dataset})).catch(() => null) : null;
  console.error('Last bilinear status:', JSON.stringify(report.lastStatus));
  throw error;
} finally {
  await writeFile(resolve(output,'report.json'),JSON.stringify(report,null,2)+'\n');
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
