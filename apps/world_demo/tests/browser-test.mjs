import {createServer} from 'node:http';
import {readFile, writeFile, mkdir} from 'node:fs/promises';
import {resolve, sep} from 'node:path';
import assert from 'node:assert/strict';
const {chromium} = await import(process.env.LUDUS_PLAYWRIGHT_MODULE || '../../../tools/web-browser-tests/node_modules/playwright/index.mjs');
const {PNG} = await import(process.env.LUDUS_PNG_MODULE || '../../../tools/web-browser-tests/node_modules/pngjs/lib/png.js');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/world-browser');
await mkdir(output, {recursive:true});
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  const path = resolve(root, '.' + (pathname === '/' ? '/index.html' : pathname));
  if (!path.startsWith(root + sep)) {response.writeHead(403).end(); return;}
  try {const data = await readFile(path); response.writeHead(200, {'Content-Type': path.endsWith('.wasm') ? 'application/wasm' : path.endsWith('.js') ? 'text/javascript' : 'text/html'}).end(data);}
  catch {response.writeHead(404).end();}
});
await new Promise(done => server.listen(0, '127.0.0.1', done));
const report = {adapter:'Chromium SwiftShader software GPU; hardware performance not measured', checks:[], errors:[]};
let browser;
function check(message) {report.checks.push(message); console.log(message);}
try {
  browser = await chromium.launch({headless:true, args:['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--use-vulkan=swiftshader', '--disable-vulkan-surface', '--enable-unsafe-swiftshader']});
  report.browser = browser.version();
  const page = await browser.newPage({viewport:{width:640,height:480}});
  page.on('pageerror', error => report.errors.push(String(error)));
  page.on('console', message => {if (message.type() === 'error') report.errors.push(message.text());});
  if (process.env.LUDUS_WORLD_WEBGL === '1') await page.addInitScript(() => Object.defineProperty(navigator, 'gpu', {value:undefined}));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'playing', {}, {timeout:30000});
  report.backend = await page.locator('#status').getAttribute('data-backend');
  assert.equal(report.backend, process.env.LUDUS_WORLD_WEBGL === '1' ? 'webgl2' : 'webgpu');
  const state = () => page.locator('#status').evaluate(node => ({...node.dataset}));
  // The app runs on requestAnimationFrame, so two page frames include at least one app frame.
  const frames = () => page.evaluate(() => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done))));
  // An input reset discards queued records (K05). Software-GPU frames can exceed a fixed sleep, so wait
  // for the published mode before blurring instead of assuming a key press has been consumed.
  const mode = expected => page.waitForFunction(value => document.querySelector('#status').dataset.mode === value, expected);
  await page.locator('canvas').evaluate(node => node.focus()); await page.waitForTimeout(100);
  await page.keyboard.press('p');
  await page.waitForFunction(() => document.querySelector('#status').dataset.mode === 'paused');
  const paused = await state(); await page.waitForTimeout(150); assert.equal((await state()).tick, paused.tick);
  const canvas = await page.locator('canvas').boundingBox();
  const scene = await page.screenshot({timeout:60000,path:resolve(output, 'scene.png'), clip:canvas});
  const pixels = PNG.sync.read(scene);
  const colors = {player:0, guard:0, exit:0};
  for(let index=0; index<pixels.data.length; index+=4) {
    const [r,g,b] = pixels.data.subarray(index,index+3);
    if(r>230 && g>120 && g<230 && b<100) ++colors.player;
    if(r>200 && g<100 && b<110) ++colors.guard;
    if(g>200 && r<120 && b>100 && b<180) ++colors.exit;
  }
  assert.ok(colors.player>40 && colors.guard>40 && colors.exit>40, 'Presented scene must contain player, guard and exit pixels: '+JSON.stringify(colors));
  report.scenePixels = colors; check('visible player, guard and exit; pause freezes ticks');
  // The room's scatter placements match the native fixture (apps/world_demo/tests/scatter_tests.cpp) bit for bit.
  assert.ok(Number(paused.placements) > 0, 'First room must prepare scatter placements');
  assert.equal(paused.scatter, '599315e43924865a', 'Browser scatter digest must equal the pinned native digest');
  report.scatter = {placements:Number(paused.placements), digest:paused.scatter}; check('scatter placements match the pinned native digest');
  await page.keyboard.press('n');
  await page.waitForFunction(tick => Number(document.querySelector('#status').dataset.tick) >= tick, Number(paused.tick)+1);
  await frames(); await page.waitForTimeout(100); assert.equal(Number((await state()).tick), Number(paused.tick)+1); check('single step advances exactly one neutral tick');
  await page.keyboard.press('p'); await mode('playing');
  const before = await state(); await page.keyboard.down('d');
  await page.waitForFunction(x => Number(document.querySelector('#status').dataset.x) > x + 0.5, Number(before.x), {timeout:30000});
  await page.keyboard.up('d');
  assert.ok(Number((await state()).x) > Number(before.x)+0.5); check('held movement advances world coordinates');
  await page.keyboard.press('Space'); await page.keyboard.press('p'); await mode('paused');
  await page.locator('canvas').evaluate(node => node.blur()); await page.locator('canvas').evaluate(node => node.focus()); await frames(); await page.waitForTimeout(100); assert.equal((await state()).mode, 'paused'); check('focus loss and regain preserve pause');
  await page.keyboard.press('r'); await page.waitForTimeout(180); await page.waitForFunction(() => Number(document.querySelector('#status').dataset.x) < -5.9); const restarted = await state(); assert.ok(Number(restarted.x) < -5.9); assert.equal(restarted.mode, 'playing'); check('restart publishes a fresh initial level');
  // A held control is cancelled by blur and stays suppressed through refocus.
  await page.keyboard.down('d'); await page.waitForTimeout(80); await page.locator('canvas').evaluate(node => node.blur()); await page.waitForTimeout(80); await page.locator('canvas').evaluate(node => node.focus());
  const refocused = await state(); await page.waitForTimeout(120); assert.equal((await state()).x, refocused.x); await page.keyboard.up('d'); check('focus cancellation prevents retained movement');
  await page.setViewportSize({width:480,height:740}); await page.waitForTimeout(150); await page.screenshot({timeout:60000,path:resolve(output,'narrow.png')}); check('narrow resize presents the scene');
  // Explicit opt-in follows the drawn player, including pause and resize.
  await page.goto(`http://127.0.0.1:${server.address().port}/?camera=follow`);
  await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'playing', {}, {timeout:30000});
  await page.locator('canvas').evaluate(node => node.focus()); await page.waitForTimeout(100);
  await page.keyboard.press('p');
  await page.waitForFunction(() => document.querySelector('#status').dataset.mode === 'paused');
  for (const viewport of [{width:640,height:480}, {width:480,height:740}]) {
    await page.setViewportSize(viewport); await page.waitForTimeout(150);
    const clip = await page.locator('canvas').boundingBox();
    const pixels = PNG.sync.read(await page.screenshot({timeout:60000,clip,path:resolve(output, `follow-${viewport.width}.png`)}));
    let count = 0, sumX = 0, sumY = 0;
    for (let y=0; y<pixels.height; ++y) for (let x=0; x<pixels.width; ++x) {
      const index=(y*pixels.width+x)*4, [r,g,b]=pixels.data.subarray(index,index+3);
      if (r>230 && g>120 && g<230 && b<100) {++count; sumX+=x+0.5; sumY+=y+0.5;}
    }
    assert.ok(count>40, 'Follow frame must contain player pixels');
    assert.ok(Math.abs(sumX/count-pixels.width/2)<2 && Math.abs(sumY/count-pixels.height/2)<2,
      `Follow camera must center drawn player: ${sumX/count},${sumY/count} in ${pixels.width}x${pixels.height}`);
  }
  check('opt-in follow centers the interpolated player on wide and narrow viewports');
  assert.equal(report.errors.length, 0); report.outcome = 'passed';
} catch(error) {report.outcome='failed'; report.failure=String(error); process.exitCode=1;}
finally {await writeFile(resolve(output,'report.json'), JSON.stringify(report,null,2)+'\n'); console.log(JSON.stringify(report)); await browser?.close(); server.close();}
