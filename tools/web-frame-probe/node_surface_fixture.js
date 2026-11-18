// Explicit test provider for the R14b surface probe (surfaces.cpp); real browsers use their
// own DOM and navigator.gpu. Every canvas owns one recording WebGPU context, and the probe
// injects delayed or failing configuration results, delayed queue completion and canvas
// removal through Module.surfaceFixture.
if (typeof window === 'undefined' && typeof process === 'object') {
  function assert(ok, message) {if (!ok) throw new Error(message);}
  class Target { addEventListener() {} removeEventListener() {} }
  const canvases = new Map();
  const records = new Map();
  let currentDevice = null;
  function makeCanvas(selector, kind) {
    const canvas = new Target();
    Object.assign(canvas, {width: 300, height: 150, clientWidth: 300, clientHeight: 150, isConnected: true});
    const record = {configured: 0, configures: 0, unconfigures: 0, acquired: 0, clear: -1};
    const context = kind === 'webgpu' ? {canvas,
      configure(config) {
        assert(config.device === currentDevice && config.format === 'bgra8unorm', 'surface configuration');
        ++record.configures; record.configured = 1;
      },
      unconfigure() { ++record.unconfigures; record.configured = 0; },
      getCurrentTexture() {
        assert(record.configured === 1 && canvas.width > 0 && canvas.height > 0, 'acquisition while unconfigured');
        ++record.acquired;
        return {kind: 'frame texture', canvas, createView() {return {kind: 'frame view', canvas};}};
      }
    } : null;
    canvas.getContext = requested => requested === kind ? (context ?? {kind}) : null;
    canvas.getAttribute = () => null;
    canvases.set(selector, canvas);
    records.set(selector, record);
  }
  for (const selector of ['#canvas', '#second', '#third', '#fourth', '#fifth']) makeCanvas(selector, 'webgpu');
  makeCanvas('#webgl', 'webgl2');
  function selectorOf(canvas) {
    for (const [selector, value] of canvases) if (value === canvas) return selector;
    return '';
  }
  globalThis.window = new Target();
  globalThis.document = new Target(); document.hidden = false;
  document.querySelector = selector => canvases.get(selector) ?? null;
  globalThis.HTMLCanvasElement = class {static [Symbol.hasInstance](value) {return [...canvases.values()].includes(value);}};
  globalThis.requestAnimationFrame = callback => setTimeout(() => callback(performance.now()), 8);
  globalThis.GPUValidationError = class extends Error {};
  globalThis.GPUOutOfMemoryError = class extends Error {};
  globalThis.GPUInternalError = class extends Error {};
  const limits = () => new Proxy({maxTextureDimension2D: 4096, maxUniformBufferBindingSize: 65536, maxBufferSize: 268435456, maxTextureDimension3D: 2048, maxTextureArrayLayers: 256, maxStorageBufferBindingSize: 134217728, maxBindingsPerBindGroup: 1000, maxUniformBuffersPerShaderStage: 12, maxStorageBuffersPerShaderStage: 8, maxSampledTexturesPerShaderStage: 16, maxSamplersPerShaderStage: 16}, {get: (o, k) => o[k] ?? 0});
  // Results of the next popErrorScope calls, oldest first; an empty queue resolves cleanly.
  const scopes = [];
  let completionDelay = 0;
  function device() {
    let lose;
    const result = {
      features: new Set(), limits: limits(),
      queue: {
        submit(commands) {assert(commands.length === 1 && commands[0].kind === 'frame commands', 'submission');},
        writeBuffer() {},
        onSubmittedWorkDone() {return new Promise(resolve => setTimeout(resolve, completionDelay));}
      },
      lost: new Promise(resolve => {lose = resolve;}),
      pushErrorScope() {},
      popErrorScope() {
        const next = scopes.shift() ?? {shouldFail: false, delay: 0};
        return new Promise(resolve => setTimeout(() => resolve(next.shouldFail ? new GPUValidationError('injected configuration failure') : null), next.delay));
      },
      createTexture(desc) {
        assert(desc.format === 'depth24plus', 'surface depth format');
        return {kind: 'frame depth texture', createView() {return {kind: 'frame depth view'};}, destroy() {}};
      },
      createCommandEncoder() {
        let ended = true;
        return {kind: 'frame encoder',
          beginRenderPass(desc) {
            const view = desc.colorAttachments[0].view;
            assert(view.kind === 'frame view' && desc.colorAttachments[0].loadOp === 'clear', 'surface clear pass');
            records.get(selectorOf(view.canvas)).clear = Math.round(desc.colorAttachments[0].clearValue.r * 100);
            ended = false;
            return {kind: 'frame pass', end() {ended = true;}};
          },
          finish() {assert(ended, 'unfinished pass'); return {kind: 'frame commands'};}
        };
      },
      destroy() {if (!result.destroyed) {result.destroyed = true; lose({reason: 'destroyed', message: 'probe destroy'});}}
    };
    currentDevice = result;
    return result;
  }
  const adapter = {features: new Set(), limits: limits(), requestDevice: () => Promise.resolve(device())};
  Object.defineProperty(globalThis, 'navigator', {value: {gpu: {getPreferredCanvasFormat: () => 'bgra8unorm', requestAdapter: () => Promise.resolve(adapter)}}});
  let isFinished = false;
  function finish(isPassed, label) {
    if (isFinished) return;
    isFinished = true;
    console.log('[R14b-web:' + (isPassed ? 'passed' : 'failed') + '] ' + label);
    process.exit(isPassed ? 0 : 1);
  }
  Module.surfaceFixture = {
    setNextScope(shouldFail, delay) {scopes.push({shouldFail, delay});},
    setCompletionDelay(delay) {completionDelay = delay;},
    disconnect(selector) {canvases.get(selector).isConnected = false;},
    read(selector, key) {
      const canvas = canvases.get(selector);
      return key === 'width' ? canvas.width : key === 'height' ? canvas.height : records.get(selector)[key];
    },
    // Read-only inspection of the pinned port's handle registry verifies C API release.
    hasFrameHandles() {return Object.values(WebGPU.Internals.jsObjects).some(object => object?.kind?.startsWith('frame')) ? 1 : 0;},
    finish
  };
  setTimeout(() => finish(false, 'timeout'), 5000);
}
