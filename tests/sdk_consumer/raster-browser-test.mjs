// Installed-SDK raster conformance. Real shader compilation, indexed instances,
// depth, texture origin and retained snapshots on both forced APIs and Auto.
import assert from 'node:assert/strict';
import {readFile, writeFile, mkdir, access} from 'node:fs/promises';
import {createServer} from 'node:http';
import {resolve, sep} from 'node:path';
import {createRequire} from 'node:module';
const require = createRequire(new URL('../../tools/web-browser-tests/package.json', import.meta.url));
const {chromium} = require('playwright');
const {PNG} = require('pngjs');
const root = resolve(process.argv[2]);
const output = resolve(process.argv[3] || 'out/raster-browser');
await mkdir(output, {recursive:true});
for (const name of ['raster.html','raster.js','raster.wasm','r6.html','r6.js','r6.wasm','r8.html','r8.js','r8.wasm','r9.html','r9.js','r9.wasm','r10.html','r10.js','r10.wasm','r11.html','r11.js','r11.wasm','r18c.html','r18c.js','r18c.wasm']) await access(resolve(root,name));
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
let browser, lastPage;
try {
  assert.equal((await fetch(`http://127.0.0.1:${server.address().port}/missing-asset`)).status,404);
  browser = await chromium.launch({headless:true, args});
  const pin = JSON.parse(await readFile(new URL('../../config/shader_toolchain.json',import.meta.url))).wgsl_validator;
  assert.equal(browser.version(),pin.chromium_version);
  report.version = browser.version();
  for (const scenario of ['webgpu-timing-merge','webgpu-timing-loss-merge','webgpu-no-timestamps-timing-merge','webgl2-timing-merge','webgl2-disjoint-timing-merge','webgpu','webgl2','auto','auto-fallback','webgpu-profile-limit-failure','webgl2-profile-limit-failure','auto-profile-limit-fallback','webgpu-stride-256','webgl2-stride-252','webgl2-stride-profile-failure','webgpu-srgb','webgl2-srgb','webgpu-blend','webgl2-blend','webgpu-graph','webgl2-graph','webgpu-graph-reference','webgl2-graph-reference','webgpu-merge','webgpu-merge-reference','webgl2-merge','webgl2-merge-reference','auto-fallback-merge','webgpu-cpu','webgpu-compute','webgpu-compute-reference','webgl2-cpu','webgl2-compute','auto-fallback-compute','webgl2-compute-required','invalid-shader','invalid-pipeline','invalid-upload','invalid-readback','slow-completion','transfer-loss','device-loss']) {
    const context = await browser.newContext({viewport:{width:200,height:160},deviceScaleFactor:1});
    await context.addInitScript(scenario => {
      window.__qaPause = false;
      window.__qaRequiredStride = scenario === 'webgl2-stride-252' ? 252 : scenario.includes('stride') ? 256 : 0;
      window.__qaSlowTransfers = scenario === 'slow-completion';
      if (scenario.startsWith('auto-fallback')) Object.defineProperty(navigator,'gpu',{get:() => undefined});
      if (navigator.gpu) {
        const requestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
        navigator.gpu.requestAdapter = async (...args) => {
          const adapter = await requestAdapter(...args);
          if (!adapter) return adapter;
          if (scenario.includes('no-timestamps')) {
            Object.defineProperty(adapter, 'features', {value:new Set([...adapter.features].filter(name => name !== 'timestamp-query'))});
          }
          const requestDevice = adapter.requestDevice.bind(adapter);
          adapter.requestDevice = async (...args) => {
            const device = await requestDevice(...args); window.__qaDevice = device;
            if (scenario === 'webgpu-profile-limit-failure' || scenario === 'auto-profile-limit-fallback') {
              const limits = device.limits;
              Object.defineProperty(device,'limits',{value:new Proxy(limits,{get:(object,key) =>
                key === 'maxVertexBuffers' ? 1 : Reflect.get(object,key,object)})});
              window.__qaCreates = 0;
            }
            const shader = device.createShaderModule.bind(device);
            device.createShaderModule = desc => shader(scenario === 'invalid-shader' ? {...desc,code:'invalid WGSL'} : desc);
            const pipeline = device.createRenderPipelineAsync.bind(device);
            device.createRenderPipelineAsync = desc => pipeline(scenario === 'invalid-pipeline' ? {...desc,vertex:{...desc.vertex,entryPoint:'missing'}} : desc);
            const buffer = device.createBuffer.bind(device);
            device.createBuffer = desc => {
              window.__qaCreates = (window.__qaCreates || 0) + 1;
              const object = buffer(scenario === 'invalid-upload' && (desc.usage & GPUBufferUsage.VERTEX) && (desc.usage & GPUBufferUsage.COPY_DST) ? {...desc,size:4} : desc);
              if (scenario.includes('timing-loss') && desc.size === 32 && (desc.usage & GPUBufferUsage.MAP_READ)) {
                const map = object.mapAsync.bind(object);
                object.mapAsync = (...args) => map(...args).then(() => new Promise(resolve => setTimeout(() => {
                  window.__qaTimingMapDelivered = true; resolve();
                },2000)));
              }
              if (scenario === 'invalid-readback' && (desc.usage & GPUBufferUsage.MAP_READ)) {
                window.__qaReadbackStarted = true;
                window.__qaReadbackCompletionObserved = false;
                const map = object.mapAsync.bind(object);
                object.mapAsync = (mode, offset, size) => map(mode, offset + 1, size);
              }
              return object;
            };
            const done = device.queue.onSubmittedWorkDone.bind(device.queue);
            device.queue.onSubmittedWorkDone = (...args) => {
              const promise = done(...args);
              if (scenario === 'transfer-loss') { device.destroy(); }
              if (scenario === 'invalid-readback' && window.__qaReadbackStarted) {
                return promise.then(() => new Promise(resolve => setTimeout(() => {
                  window.__qaReadbackCompletionObserved = true;
                  resolve();
                },100)));
              }
              return scenario === 'slow-completion' ? promise.then(() => new Promise(resolve => setTimeout(resolve,30))) : promise;
            };
            return device;
          };
          return adapter;
        };
      }
      if (scenario === 'webgl2-profile-limit-failure' || scenario.includes('disjoint')) {
        const getContext = HTMLCanvasElement.prototype.getContext;
        HTMLCanvasElement.prototype.getContext = function(...args) {
          const context = getContext.apply(this,args);
          if (args[0] === 'webgl2' && context && !context.__qaLimits) {
            context.__qaLimits = true;
            const parameter = context.getParameter.bind(context);
            context.getParameter = name => scenario.includes('disjoint') ? (name === 0x8FBB ? true : parameter(name)) : name === context.MAX_VERTEX_ATTRIBS ? 4 : parameter(name);
          }
          return context;
        };
      }
      new MutationObserver(() => {
        const frames = Number(document.querySelector('#status')?.dataset.frames);
        if (frames === 5) window.__qaPause = true;
      }).observe(document,{attributes:true,subtree:true});
    },scenario);
    const page = await context.newPage(); lastPage = page;
    const errors = [];
    page.on('pageerror',error => errors.push(String(error)));
    page.on('console',message => report.console.push({scenario,type:message.type(),text:message.text()}));
    const variant = scenario.endsWith('-merge-reference') ? 'merge-reference' : scenario.endsWith('-merge') ? 'merge' : scenario.endsWith('-compute-required') ? 'compute-required' : scenario.endsWith('-compute-reference') ? 'compute-reference' : scenario.endsWith('-compute') ? 'compute' : scenario.endsWith('-cpu') ? 'cpu' : scenario.endsWith('-graph-reference') ? 'graph-reference' : scenario.endsWith('-graph') ? 'graph' : scenario.endsWith('-srgb') ? 'srgb' : scenario.endsWith('-blend') ? 'blend' : '';
    const selection = scenario.startsWith('webgpu') ? 'webgpu' : scenario.startsWith('webgl2') ? 'webgl2' : scenario.startsWith('invalid') || scenario === 'device-loss' || scenario === 'slow-completion' || scenario === 'transfer-loss' ? 'webgpu' : 'auto';
    await page.goto(`http://127.0.0.1:${server.address().port}/raster.html?backend=${selection}&variant=${variant}${scenario.includes('timing') ? '&timing=1' : ''}`);
    const result = {scenario};
    if (scenario.startsWith('invalid') || scenario === 'transfer-loss' || scenario === 'webgl2-compute-required' || (scenario.endsWith('profile-limit-failure') || scenario.endsWith('stride-profile-failure'))) {
      await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'failed',null,{timeout:20000});
      assert.equal(await page.locator('#status').getAttribute('data-frames'),'0');
      if (scenario === 'webgpu-profile-limit-failure') assert.equal(await page.evaluate(() => window.__qaCreates),0,'admission must precede buffer creation');
      if (scenario === 'invalid-readback') assert.equal(await page.evaluate(() => window.__qaReadbackCompletionObserved),true,'map failure must not retire the outstanding copy');
    } else {
      await page.waitForFunction(() => document.querySelector('#status').dataset.frames === '5',null,{timeout:20000});
      result.backend = await page.locator('#status').getAttribute('data-backend');
      if (scenario.includes('timing') && !scenario.includes('timing-loss')) {
        await page.waitForFunction(() => ['3','4','5','6'].includes(document.querySelector('#status').dataset.timingState));
        result.timing = await page.locator('#status').evaluate(node => ({state:Number(node.dataset.timingState),gpuNs:Number(node.dataset.gpuNs),logicalBytes:Number(node.dataset.logicalBytes),apiBytesAvailable:Number(node.dataset.apiBytesAvailable)}));
        assert.ok(result.timing.logicalBytes > 0);
        assert.equal(result.timing.apiBytesAvailable,0);
        if (scenario.includes('no-timestamps')) assert.equal(result.timing.state,4);
        else if (scenario.startsWith('webgpu')) assert.equal(result.timing.state,3);
        else if (scenario.includes('disjoint')) assert.ok([4,5].includes(result.timing.state));
        if (result.timing.state === 3) assert.ok(result.timing.gpuNs >= 0);
        else assert.equal(result.timing.gpuNs,0);
      }
      if (scenario.startsWith('auto-fallback') || scenario === 'auto-profile-limit-fallback' || scenario.startsWith('webgl2')) assert.equal(result.backend,'webgl2');
      else assert.equal(result.backend,'webgpu');
      if (scenario === 'device-loss' || scenario.includes('timing-loss')) {
        await page.evaluate(() => {window.__qaDevice.destroy(); window.__qaPause=false;});
        await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'failed',null,{timeout:20000});
        if (scenario.includes('timing-loss')) {
          await page.waitForFunction(() => window.__qaTimingMapDelivered);
          assert.equal(await page.locator('#status').getAttribute('data-state'),'failed','late timing callback cannot revive the owner');
        }
      } else {
        const box = await page.locator('#canvas').boundingBox();
        const bytes = await page.screenshot({clip:{x:box.x,y:box.y,width:box.width,height:box.height}});
        const image = PNG.sync.read(bytes);
        assert.equal(image.width,96); assert.equal(image.height,64);
        const high = variant === 'srgb' ? 55 : variant === 'blend' ? 160 : 255;
        const low = variant === 'blend' ? 32 : 0;
        for (const [i,x] of (variant.startsWith('merge') ? [] : [12,36,60,84]).entries()) {
          const left = i%2 === 0;
          for (const [y,expected] of [[16,left?[high,low,low,255]:[low,high,low,255]],[48,left?[low,low,high,255]:[high,high,high,255]]]) {
            const offset = (y*image.width+x)*4;
            assert.deepEqual([...image.data.subarray(offset,offset+4)],expected,`${scenario} pixel ${x},${y}`);
          }
        }
        if (variant.startsWith('merge')) {
          assert.deepEqual([...image.data.subarray((16*96+12)*4,(16*96+12)*4+4)],[191,0,0,255],`${scenario} painter order red`);
          assert.deepEqual([...image.data.subarray((48*96+12)*4,(48*96+12)*4+4)],[0,0,159,255],`${scenario} painter order blue`);
          assert.equal(await page.locator('#status').getAttribute('data-live-passes'),'17');
          assert.equal(await page.locator('#status').getAttribute('data-raster-passes'),variant === 'merge' ? '2' : '17');
          if (variant === 'merge-reference') {
            const merged = PNG.sync.read(await readFile(resolve(output,scenario.replace('-reference','')+'.png')));
            assert.deepEqual(image.data,merged.data,`${scenario} painter-order complete images`);
          }
        }
        if (variant.startsWith('graph')) {
          assert.equal(await page.locator('#status').getAttribute('data-live-passes'),variant === 'graph-reference' ? '4' : '3');
          assert.ok(Number(await page.locator('#status').getAttribute('data-dependencies')) >= 2);
          if (variant === 'graph-reference') {
            const optimized = PNG.sync.read(await readFile(resolve(output,scenario.replace('-reference','')+'.png')));
            assert.deepEqual(image.data,optimized.data,`${scenario} reference and optimized complete images`);
          }
        }
        if (variant === 'compute' || variant === 'compute-reference') {
          const gpu = result.backend === 'webgpu';
          assert.equal(await page.locator('#status').getAttribute('data-live-passes'), gpu ? (variant === 'compute-reference' ? '5' : '4') : '3');
          const cpu = PNG.sync.read(await readFile(resolve(output,(gpu ? 'webgpu-cpu' : 'webgl2-cpu')+'.png')));
          assert.deepEqual(image.data,cpu.data,`${scenario} compute/CPU complete images`);
        }
        await writeFile(resolve(output,`${scenario}.png`),bytes);
        await page.evaluate(() => {window.__qaPause=false;});
        await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'passed',null,{timeout:20000});
        assert.equal(await page.locator('#status').getAttribute('data-frames'),'120');
      }
    }
    assert.deepEqual(errors,[]); result.outcome='passed'; report.cases.push(result);
    await context.close();
  }
  report.outcome='passed';
  for (const backend of ['webgpu','webgl2','auto','fallback']) {
    for (const reference of [false,true]) {
      const scenario = `r6-${backend}${reference ? '-reference' : ''}`;
      const context = await browser.newContext({viewport:{width:200,height:160},deviceScaleFactor:1});
      await context.addInitScript(fallback => {
        window.__qaPause = false;
        if (fallback) Object.defineProperty(navigator,'gpu',{value:undefined,configurable:true});
        new MutationObserver(() => {
          if (document.querySelector('#status')?.dataset.frames === '1') window.__qaPause = true;
        }).observe(document,{attributes:true,subtree:true});
      }, backend === 'fallback');
      const page = await context.newPage(); lastPage = page;
      const errors = [];
      page.on('pageerror',error => errors.push(String(error)));
      page.on('console',message => report.console.push({scenario,type:message.type(),text:message.text()}));
      const selection = backend === 'fallback' ? 'auto' : backend;
      await page.goto(`http://127.0.0.1:${server.address().port}/r6.html?backend=${selection}&reference=${reference ? '1' : '0'}`);
      await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'rendering' && window.__qaPause,null,{timeout:30000});
      assert.equal(await page.locator('#status').getAttribute('data-passes'),'40');
      assert.equal(await page.locator('#status').getAttribute('data-draws'),'405');
      assert.equal(await page.locator('#status').getAttribute('data-compute'),backend === 'webgl2' || backend === 'fallback' ? '0' : '1');
      const bytes = await page.locator('canvas').screenshot();
      const image = PNG.sync.read(bytes);
      for (const [i,x] of [12,36,60,84].entries()) {
        for (const [y,expected] of [[16,i%2 === 0 ? [255,0,0,255] : [0,255,0,255]],[48,i%2 === 0 ? [0,0,255,255] : [255,255,255,255]]]) {
          const offset = (y*image.width+x)*4;
          assert.deepEqual([...image.data.subarray(offset,offset+4)],expected,`${scenario} pixel ${x},${y}`);
        }
      }
      if (reference) {
        const graph = PNG.sync.read(await readFile(resolve(output,`r6-${backend}.png`)));
        assert.deepEqual(image.data,graph.data,`${scenario} graph/direct complete images`);
      }
      await writeFile(resolve(output,`${scenario}.png`),bytes);
      await page.evaluate(() => {window.__qaPause=false;});
      await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'passed',null,{timeout:30000});
      assert.equal(await page.locator('#status').getAttribute('data-frames'),'3');
      assert.deepEqual(errors,[]);
      report.cases.push({scenario,outcome:'passed',objectsPerKind:72,graphDraws:405,batchDraws:1100});
      await context.close();
    }
  }

  for (const backend of ['webgpu','webgl2','auto','fallback','webgpu-invalid-copy','webgpu-loss']) {
    const scenario = `r8-${backend}`;
    const context = await browser.newContext({viewport:{width:200,height:160},deviceScaleFactor:1});
    await context.addInitScript(fallback => {
      window.__qaPause = false;
      if (fallback === 'fallback') Object.defineProperty(navigator,'gpu',{value:undefined,configurable:true});
      if (navigator.gpu && (fallback === 'webgpu-invalid-copy' || fallback === 'webgpu-loss')) {
        const requestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
        navigator.gpu.requestAdapter = async (...args) => {
          const adapter = await requestAdapter(...args);
          if (!adapter) return adapter;
          const requestDevice = adapter.requestDevice.bind(adapter);
          adapter.requestDevice = async (...args) => {
            const device = await requestDevice(...args);
            if (fallback === 'webgpu-invalid-copy') {
              const createEncoder = device.createCommandEncoder.bind(device);
              device.createCommandEncoder = (...args) => {
                const encoder = createEncoder(...args);
                const copy = encoder.copyTextureToTexture.bind(encoder);
                encoder.copyTextureToTexture = (source,destination,size) => copy(source,destination,{...size,width:size.width+1});
                return encoder;
              };
            } else {
              const done = device.queue.onSubmittedWorkDone.bind(device.queue);
              let started = false;
              device.queue.onSubmittedWorkDone = (...args) => {
                const promise = done(...args);
                if (!started) {
                  started = true;
                  setTimeout(() => device.destroy(),100);
                  return promise.then(() => new Promise(resolve => setTimeout(() => {
                    window.__qaDelayedTransferDone = true; resolve();
                  },700)));
                }
                return promise;
              };
            }
            return device;
          };
          return adapter;
        };
      }
      new MutationObserver(() => {
        if (document.querySelector('#status')?.dataset.frames === '1') window.__qaPause = true;
      }).observe(document,{attributes:true,subtree:true});
    }, backend);
    const page = await context.newPage(); lastPage = page;
    const errors = [];
    page.on('pageerror',error => errors.push(String(error)));
    page.on('console',message => report.console.push({scenario,type:message.type(),text:message.text()}));
    await page.goto(`http://127.0.0.1:${server.address().port}/r8.html?backend=${backend === 'fallback' ? 'auto' : backend.startsWith('webgpu-') ? 'webgpu' : backend}`);
    if (backend === 'webgpu-invalid-copy' || backend === 'webgpu-loss') {
      await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'failed',null,{timeout:30000});
      if (backend === 'webgpu-loss') {
        await page.waitForFunction(() => window.__qaDelayedTransferDone,null,{timeout:5000});
      }
      assert.deepEqual(errors,[]);
      report.cases.push({scenario,outcome:'passed',expectedFailure:true});
      await context.close();
      continue;
    }
    await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'rendering' && window.__qaPause,null,{timeout:30000});
    const bytes = await page.locator('canvas').screenshot();
    const image = PNG.sync.read(bytes);
    for (const [x,y,color] of [[24,16,[255,0,0,255]],[72,16,[0,255,0,255]],[24,48,[0,0,255,255]],[72,48,[255,255,255,255]]]) {
      const offset = (y*image.width+x)*4;
      assert.deepEqual([...image.data.subarray(offset,offset+4)],color,`${scenario} mutable pixel ${x},${y}`);
    }
    if (backend !== 'webgpu') {
      const reference = PNG.sync.read(await readFile(resolve(output,'r8-webgpu.png')));
      assert.deepEqual(image.data,reference.data,`${scenario} complete updated images`);
    }
    await writeFile(resolve(output,`${scenario}.png`),bytes);
    await page.evaluate(() => {window.__qaPause=false;});
    await page.waitForFunction(() => document.querySelector('#status').dataset.state === 'passed',null,{timeout:30000});
    assert.deepEqual(errors,[]);
    report.cases.push({scenario,outcome:'passed',initializedMips:3,textureReadback:true,renderedReadback:true});
    await context.close();
  }

  {
    const context = await browser.newContext();
    const page = await context.newPage(); lastPage = page;
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => report.console.push({scenario:'r18c-webgpu',type:message.type(),text:message.text()}));
    await page.goto(`http://127.0.0.1:${server.address().port}/r18c.html`);
    await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:60000});
    const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
    assert.equal(status.state,'passed',JSON.stringify(status));
    assert.deepEqual(errors,[]);
    report.cases.push({scenario:'r18c-webgpu',bufferBytes:67109120,storageRangeBytes:16777472,storageBindings:5,totalBindings:9,gpuValue:20});
    await context.close();
  }
  for (const backend of ['webgpu','webgl2','auto','fallback','webgl2-volume-256','webgl2-dimension-2048']) {
    const context = await browser.newContext();
    if (backend === 'fallback') await context.addInitScript(() => Object.defineProperty(navigator,'gpu',{get:() => undefined}));
    if (backend === 'webgl2-volume-256') await context.addInitScript(() => {
      const getParameter = WebGL2RenderingContext.prototype.getParameter;
      WebGL2RenderingContext.prototype.getParameter = function(parameter) {
        return parameter === this.MAX_3D_TEXTURE_SIZE ? 256 : getParameter.call(this, parameter);
      };
    });
    if (backend === 'webgl2-dimension-2048') await context.addInitScript(() => {
      const getParameter = WebGL2RenderingContext.prototype.getParameter;
      WebGL2RenderingContext.prototype.getParameter = function(parameter) {
        return parameter === this.MAX_TEXTURE_SIZE ? 2048 : getParameter.call(this, parameter);
      };
    });
    const page = await context.newPage(); lastPage = page;
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => report.console.push({scenario:`r9-${backend}`,type:message.type(),text:message.text()}));
    await context.addInitScript(() => {
      globalThis.__textureLimitRequests = [];
      if (typeof GPUAdapter !== 'undefined') {
        const request = GPUAdapter.prototype.requestDevice;
        GPUAdapter.prototype.requestDevice = function(descriptor) {
          globalThis.__textureLimitRequests.push({
            adapter: this.limits.maxTextureDimension2D,
            buffer: this.limits.maxBufferSize,
            storage: this.limits.maxStorageBufferBindingSize,
            layers: this.limits.maxTextureArrayLayers,
            volume: this.limits.maxTextureDimension3D,
            limits: {...descriptor.requiredLimits},
            requested: descriptor.requiredLimits?.maxTextureDimension2D,
          });
          return request.call(this, descriptor);
        };
      }
    });
    await page.goto(`http://127.0.0.1:${server.address().port}/r9.html?backend=${backend === 'fallback' ? 'auto' : backend.startsWith('webgl2') ? 'webgl2' : backend}`);
    await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:45000});
    const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
    assert.equal(status.state,'passed',JSON.stringify(status));
    assert.equal(status.frames,'20'); assert.deepEqual(errors,[]);
    const textureLimitRequests = await page.evaluate(() => globalThis.__textureLimitRequests);
    if (backend === 'webgpu' || backend === 'auto') {
      assert.equal(textureLimitRequests.length, 1);
      assert.equal(textureLimitRequests[0].requested, textureLimitRequests[0].adapter,
        'R18 must request the adapter texture maximum before publishing enabled device limits');
      const request = textureLimitRequests[0];
      assert.equal(request.limits.maxBufferSize, request.buffer);
      assert.equal(request.limits.maxStorageBufferBindingSize, Math.min(request.storage,request.buffer));
      assert.equal(request.limits.maxTextureArrayLayers, request.layers);
      assert.equal(request.limits.maxTextureDimension3D, request.volume);
      assert.equal(request.limits.maxBindingsPerBindGroup, undefined);
      assert.ok(request.limits.maxStorageBuffersPerShaderStage >= 5);
    } else {
      assert.deepEqual(textureLimitRequests, []);
    }
    if (backend === 'webgl2-volume-256') {
      assert.ok(report.console.some(message => message.scenario === `r9-${backend}` && message.text.includes('volume depth 256')));
    }
    if (backend === 'webgl2-dimension-2048') {
      assert.ok(report.console.some(message => message.scenario === `r9-${backend}` && message.text.includes('sampled width 2048 device limit 2048')));
      assert.ok(report.console.some(message => message.scenario === `r9-${backend}` && message.text.includes('complete upload 67117056 bytes')));
    }
    report.cases.push({scenario:`r9-${backend}`,frames:20,pixelReadbacks:20,textureLimitRequests});
    await context.close();
  }
  for (const backend of ['webgpu','webgl2','auto','fallback']) {
    const context = await browser.newContext();
    if (backend === 'fallback') await context.addInitScript(() => Object.defineProperty(navigator,'gpu',{get:() => undefined}));
    const page = await context.newPage(); lastPage = page;
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => report.console.push({scenario:`r10-${backend}`,type:message.type(),text:message.text()}));
    await page.goto(`http://127.0.0.1:${server.address().port}/r10.html?backend=${backend === 'fallback' ? 'auto' : backend}`);
    await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:45000});
    const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
    assert.equal(status.state,'passed',JSON.stringify(status));
    const pixelReadbacks = 21;
    assert.equal(status.frames,String(pixelReadbacks)); assert.deepEqual(errors,[]);
    report.cases.push({scenario:`r10-${backend}`,frames:pixelReadbacks,pixelReadbacks});
    await context.close();
  }

  for (const backend of ['webgpu','webgl2']) {
    const context = await browser.newContext();
    const page = await context.newPage(); lastPage = page;
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => report.console.push({scenario:`r11-${backend}`,type:message.type(),text:message.text()}));
    await page.goto(`http://127.0.0.1:${server.address().port}/r11.html?backend=${backend}`);
    await page.waitForFunction(() => ['passed','failed'].includes(document.querySelector('#status')?.dataset.state),null,{timeout:30000});
    const status = await page.locator('#status').evaluate(node => ({...node.dataset}));
    assert.equal(status.state,'passed',JSON.stringify(status));
    assert.equal(status.frames,backend === 'webgpu' ? '3' : '2'); assert.deepEqual(errors,[]);
    assert.equal(status.compute,backend === 'webgpu' ? 'passed' : 'unavailable');
    report.cases.push({scenario:`r11-${backend}`,frames:Number(status.frames),pixelReadbacks:2,compute:status.compute});
    await context.close();
  }

} catch (error) {
  report.outcome='failed'; report.error=String(error);
  report.lastStatus = lastPage ? await lastPage.locator('#status').evaluate(node => ({text:node.textContent,...node.dataset})) : null;
  throw error;
} finally {
  await writeFile(resolve(output,'report.json'),JSON.stringify(report,null,2)+'\n');
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
