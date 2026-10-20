// Explicit test provider: real browsers always use their own DOM/navigator.gpu.
if (typeof window === 'undefined' && typeof process === 'object') {
  const scenario = process.argv[2] || 'success';
  const stats = {devices: 0, destroyed: 0, configured: 0, unconfigured: 0, acquired: 0, submitted: 0, drawn: 0};
  const colors = new Set();
  const status = {textContent: '', dataset: {}};
  class Target { addEventListener() {} removeEventListener() {} }
  const canvas = new Target();
  Object.assign(canvas, {width: 640, height: 360, clientWidth: 640, clientHeight: 360, isConnected: true});
  canvas.getAttribute = () => null; canvas.setAttribute = () => {}; canvas.removeAttribute = () => {};
  canvas.getBoundingClientRect = () => ({left: 0, top: 0, width: canvas.clientWidth, height: canvas.clientHeight});
  let configured = false;
  const context = {canvas,
    configure(config) {
      assert(config.device && config.format === 'bgra8unorm', 'surface configuration');
      ++stats.configured; configured = true;
    },
    unconfigure() { ++stats.unconfigured; configured = false; },
    getCurrentTexture() {
      assert(configured && canvas.width > 0 && canvas.height > 0, 'acquisition while hidden/unconfigured');
      if (scenario === 'acquisition-failure') throw new Error('injected acquisition failure');
      ++stats.acquired;
      return {kind: 'frame texture', createView() {return {kind: 'frame view'};}};
    }
  };
  canvas.getContext = kind => kind === 'webgpu' ? context : null;
  globalThis.window = new Target();
  globalThis.document = new Target(); document.hidden = false; document.activeElement = null;
  document.querySelector = selector => selector === '#canvas' ? canvas : null;
  document.getElementById = id => id === 'status' ? status : null;
  globalThis.HTMLCanvasElement = class {static [Symbol.hasInstance](value) {return value === canvas;}};
  globalThis.ResizeObserver = class {observe() {} disconnect() {}};
  globalThis.getComputedStyle = () => ({visibility: 'visible', display: 'block'});
  globalThis.devicePixelRatio = 1;
  globalThis.requestAnimationFrame = callback => setTimeout(() => callback(performance.now()), 8);
  globalThis.GPUValidationError = class extends Error {};
  globalThis.GPUOutOfMemoryError = class extends Error {};
  globalThis.GPUInternalError = class extends Error {};
  let currentDevice;
  let isOffscreen = false;
  function device() {
    let lose;
    let scopes = 0;
    const result = {
      features: new Set(), limits: new Proxy({maxTextureDimension2D: 4096, maxUniformBufferBindingSize: 65536, maxBufferSize: 268435456, maxTextureDimension3D:2048,maxTextureArrayLayers:256,maxStorageBufferBindingSize:134217728,maxBindingsPerBindGroup:1000,maxUniformBuffersPerShaderStage:12,maxStorageBuffersPerShaderStage:8,maxSampledTexturesPerShaderStage:16,maxSamplersPerShaderStage:16}, {get: (o, k) => o[k] ?? 0}),
      queue: {submit(commands) {assert(commands.length === 1 && commands[0].ended, 'submit unfinished pass'); ++stats.submitted;}},
      lost: new Promise(resolve => {lose = resolve;}),
      pushErrorScope() {},
      popErrorScope() {
        const failure = (++scopes === 1 && scenario === 'surface-failure') || (scopes === 2 && scenario === 'shader-failure');
        return new Promise(resolve => setTimeout(() => resolve(failure ? new GPUValidationError('injected validation') : null), scenario === 'cancel-pipeline' && scopes === 2 ? 100 : 0));
      },
      createShaderModule(desc) {assert(desc.code.includes('@vertex') && desc.code.includes('@fragment'), 'WGSL missing'); return {kind: 'shader'};},
      createRenderPipeline(desc) {
        assert(desc.vertex.module && desc.fragment.targets[0].format === 'bgra8unorm', 'pipeline descriptor');
        assert(desc.depthStencil.format === 'depth24plus' && desc.depthStencil.depthWriteEnabled === false &&
               desc.depthStencil.depthCompare === 'always', 'probe depth compatibility');
        return {kind: 'pipeline'};
      },
      createTexture(desc) {
        assert(desc.format === 'depth24plus' && desc.size.width === canvas.width && desc.size.height === canvas.height,
               'depth attachment size/format');
        return {kind:'frame depth texture', createView(){return {kind:'frame depth view'};}, destroy(){}};
      },
      createCommandEncoder() {
        let ended = false;
        return {kind: 'frame encoder',
          beginRenderPass(desc) {
            assert(desc.colorAttachments.length === 1 && desc.colorAttachments[0].loadOp === 'clear', 'clear pass');
            assert(desc.depthStencilAttachment.view.kind === 'frame depth view' && desc.depthStencilAttachment.depthClearValue === 1 && desc.depthStencilAttachment.depthLoadOp === 'clear', 'depth clear pass');
            colors.add(desc.colorAttachments[0].clearValue.r);
            let bound = false;
            return {kind: 'frame pass',
              setViewport(x, y, w, h) {assert(w === h && x === (canvas.width-w)/2 && y === (canvas.height-h)/2, 'stretched viewport');},
              setPipeline(pipeline) {assert(pipeline.kind === 'pipeline', 'wrong pipeline'); bound = true;},
              draw(vertices, instances) {assert(bound && vertices === 3 && instances === 1, 'triangle draw'); ++stats.drawn;},
              end() {ended = true;}
            };
          },
          finish() {assert(ended || isOffscreen, 'unfinished pass'); return {kind: 'frame commands', ended: ended || isOffscreen};}
        };
      },
      destroy() {if (!result.destroyed) {result.destroyed = true; ++stats.destroyed; lose({reason: 'destroyed', message: 'test destroy'});}},
      testLose() {lose({reason: 'unknown', message: 'test loss'});}
    };
    ++stats.devices; currentDevice = result; return result;
  }
  const adapter = {features: new Set(), limits: new Proxy({maxTextureDimension2D: 4096, maxUniformBufferBindingSize: 65536, maxBufferSize: 268435456, maxTextureDimension3D:2048,maxTextureArrayLayers:256,maxStorageBufferBindingSize:134217728,maxBindingsPerBindGroup:1000,maxUniformBuffersPerShaderStage:12,maxStorageBuffersPerShaderStage:8,maxSampledTexturesPerShaderStage:16,maxSamplersPerShaderStage:16}, {get: (o, k) => o[k] ?? 0}), requestDevice(desc) {
    assert(!desc.requiredFeatures?.length && Object.keys(desc.requiredLimits || {}).length === 9 && desc.requiredLimits.maxTextureDimension2D === adapter.limits.maxTextureDimension2D, 'texture limit negotiation');
    return Promise.resolve(device());
  }};
  Object.defineProperty(globalThis, 'navigator', {value: {gpu: {getPreferredCanvasFormat: () => 'bgra8unorm', requestAdapter: () => Promise.resolve(adapter)}}});
  function assert(ok, message) {if (!ok) throw new Error(message);}
  function noFrameHandles() {
    // Read-only inspection of the pinned port's handle registry verifies C API
    // release, rather than just the JS mock's command methods being invoked.
    return !Object.values(WebGPU.Internals.jsObjects).some(object => object?.kind?.startsWith('frame'));
  }
  let phase = 0, checkpoint = 0;
  const timer = setInterval(() => {
    if (!Module._Stop || !status.dataset.state) return;
    const state = status.dataset.state;
    if (scenario.endsWith('failure')) {
      if (state === 'failed') {Module._Stop(); finish(stats.destroyed === stats.devices && stats.submitted === 0 && noFrameHandles(), 'failure cleanup');}
      return;
    }
    if (scenario === 'cancel-pipeline') {
      if (phase === 0 && status.textContent.includes('pipeline pending') && state === 'ready') {
        Module._Stop(); phase = 1;
        setTimeout(() => finish(status.dataset.state === 'idle' && stats.submitted === 0 && stats.destroyed === stats.devices, 'late shader callback ignored'), 150);
      }
      return;
    }
    if (stats.submitted < 2) return;
    if (scenario === 'offscreen' && phase === 0) {
      const {configured, acquired, submitted} = stats;
      // Only this explicit test call admits an empty encoder. Normal visible
      // frames must still end their clear/draw pass before submission.
      isOffscreen = true;
      assert(Module._CheckOffscreenFrame() === 1, 'offscreen frame rejected');
      isOffscreen = false;
      assert(stats.configured === configured && stats.acquired === acquired &&
             stats.submitted === submitted + 1 && noFrameHandles(), 'offscreen acquired canvas or leaked');
      checkpoint = stats.submitted;
      phase = 1;
      return;
    }
    if (scenario === 'offscreen') {
      assert(stats.submitted === stats.acquired + 1 && stats.drawn === stats.acquired, 'offscreen/visible accounting');
      if (stats.submitted > checkpoint) {
        Module._Stop();
        finish(stats.destroyed === stats.devices && noFrameHandles(), 'offscreen preserves canvas and visible frames resume');
      }
      return;
    }
    assert(noFrameHandles() && stats.submitted === stats.acquired && stats.drawn === stats.submitted, 'per-frame handle leak or incomplete submission');
    if (scenario === 'device-loss' || scenario === 'validation') {
      if (phase === 0) {
        phase = 1; checkpoint = stats.submitted;
        if (scenario === 'device-loss') currentDevice.testLose();
        else currentDevice.onuncapturederror({error: new GPUValidationError('test error')});
      } else if (phase === 1 && state === (scenario === 'device-loss' ? 'device lost' : 'failed')) {
        setTimeout(() => {Module._Stop(); finish(stats.submitted === checkpoint && stats.destroyed === stats.devices && noFrameHandles(), 'rendering stopped');}, 30);
        phase = 2;
      }
      return;
    }
    if (scenario === 'resize-hidden') {
      if (phase === 0) {canvas.clientWidth = 300; canvas.clientHeight = 150; globalThis.devicePixelRatio = 1.5; checkpoint = stats.submitted; phase = 1;}
      else if (phase === 1 && stats.submitted > checkpoint) {
        assert(canvas.width === 450 && canvas.height === 225 && stats.configured >= 3, 'DPR resize did not reconfigure');
        document.hidden = true; phase = 2;
        setTimeout(() => {checkpoint = stats.submitted; setTimeout(() => {
          assert(stats.submitted === checkpoint, 'hidden canvas rendered'); document.hidden = false; phase = 3;
        }, 40);}, 20);
      } else if (phase === 3 && stats.submitted > checkpoint) {
        Module._Stop(); finish(stats.destroyed === stats.devices && noFrameHandles(), 'resize/hide/resume');
      }
      return;
    }
    if (phase === 0) {Module._Restart(); phase = 1; checkpoint = stats.submitted;}
    else if (stats.submitted >= checkpoint + 2) {
      Module._Stop(); finish(stats.destroyed === stats.devices && noFrameHandles() && colors.size > 1, 'animated clear/triangle/restart');
    }
  }, 10);
  function finish(ok, message) {clearInterval(timer); console.log('[W5:' + (ok ? 'passed' : 'failed') + '] ' + scenario + ' ' + message); process.exit(ok ? 0 : 1);}
  setTimeout(() => finish(false, 'timeout'), 3000);
}
