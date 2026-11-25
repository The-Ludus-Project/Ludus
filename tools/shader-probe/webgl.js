// Standalone tools-only shader feasibility consumer, independent of the RHI. This never ships; it only proves the generated GLSL ES 3.00
// stages compile, link and render changing mixed-layout uniforms in real WebGL 2.
const status = document.querySelector('#status');
const report = (window.shaderProbe = {state: 'pending', cases: [], errors: [], frames: 0});
function require(condition, message) {
  if (!condition) throw new Error(message);
}
function encode(value, srgb) {
  const clamped = Math.min(1, Math.max(0, value));
  return Math.round(
    255 * (srgb ? (clamped <= 0.0031308 ? 12.92 * clamped : 1.055 * Math.pow(clamped, 1 / 2.4) - 0.055) : clamped));
}
// Mixed scalar/vector std140 upload. Byte offsets come from the generated
// contract (independently derived from the emitted GLSL ES std140 block), not
// assumed equal to the SPIR-V/WGSL layouts.
function upload(gl, buffer, contract, width, height, elapsed) {
  const bytes = new ArrayBuffer(contract.size);
  const view = new DataView(bytes);
  const values = {Resolution: [width, height], ElapsedTime: [elapsed], Direction: [0.2, 0.4, 0.6], Tint: [0.6, 0.4, 0.2, 1]};
  for (const [field, data] of Object.entries(values)) {
    data.forEach((value, i) => view.setFloat32(contract.offsets[field] + 4 * i, value, true));
  }
  gl.bindBuffer(gl.UNIFORM_BUFFER, buffer);
  gl.bufferData(gl.UNIFORM_BUFFER, bytes, gl.DYNAMIC_DRAW);
}
function compile(gl, type, source, label) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  const log = gl.getShaderInfoLog(shader) || '';
  if (log.trim()) report.compilationMessages.push({stage: label, log: log.trim()});
  require(gl.getShaderParameter(shader, gl.COMPILE_STATUS), label + ' compile failed: ' + log);
  return shader;
}
async function start() {
  report.compilationMessages = [];
  const canvas = document.querySelector('canvas');
  const gl = canvas.getContext('webgl2', {antialias: false, preserveDrawingBuffer: true});
  require(gl, 'WebGL 2 unavailable');
  const debugInfo = gl.getExtension('WEBGL_debug_renderer_info');
  report.adapter = {
    vendor: debugInfo ? gl.getParameter(debugInfo.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
    renderer: debugInfo ? gl.getParameter(debugInfo.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
    version: gl.getParameter(gl.VERSION),
    glsl: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
  };
  report.limits = {
    maxUniformBlockSize: gl.getParameter(gl.MAX_UNIFORM_BLOCK_SIZE),
    maxVertexUniformBlocks: gl.getParameter(gl.MAX_VERTEX_UNIFORM_BLOCKS),
    maxFragmentUniformBlocks: gl.getParameter(gl.MAX_FRAGMENT_UNIFORM_BLOCKS),
    maxTextureSize: gl.getParameter(gl.MAX_TEXTURE_SIZE),
    maxViewport: Array.from(gl.getParameter(gl.MAX_VIEWPORT_DIMS)),
  };

  const [vertexSource, fragmentSource, contractText] = await Promise.all(
    ['probe.vert.essl', 'probe.frag.essl', 'contract.json'].map(async name => {
      const response = await fetch(name);
      require(response.ok, 'Cannot load ' + name);
      return response.text();
    }));
  const contract = JSON.parse(contractText).glsl_es;
  require(contract.size === 48 && contract.alignment === 16, 'Unexpected upload contract');
  require(/^#version 300 es/.test(vertexSource.trimStart()), 'Vertex is not GLSL ES 3.00');
  require(/^#version 300 es/.test(fragmentSource.trimStart()), 'Fragment is not GLSL ES 3.00');

  const program = gl.createProgram();
  gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexSource, 'vertex'));
  gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentSource, 'fragment'));
  gl.linkProgram(program);
  const linkLog = gl.getProgramInfoLog(program) || '';
  if (linkLog.trim()) report.linkLog = linkLog.trim();
  require(gl.getProgramParameter(program, gl.LINK_STATUS), 'Program link failed: ' + linkLog);

  // Query the real linked program's active uniform block layout and compare it
  // with the generated contract and the upload struct. This is the authoritative
  // std140 offset/size/padding/binding check in an actual WebGL 2 program.
  const blockIndex = gl.getUniformBlockIndex(program, contract.block);
  require(blockIndex !== gl.INVALID_INDEX, 'Uniform block "' + contract.block + '" not found in linked program');
  const blockSize = gl.getActiveUniformBlockParameter(program, blockIndex, gl.UNIFORM_BLOCK_DATA_SIZE);
  const activeUniforms = gl.getActiveUniformBlockParameter(program, blockIndex, gl.UNIFORM_BLOCK_ACTIVE_UNIFORMS);
  const indices = gl.getActiveUniformBlockParameter(program, blockIndex, gl.UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES);
  const offsets = gl.getActiveUniforms(program, indices, gl.UNIFORM_OFFSET);
  const measured = {};
  for (let i = 0; i < indices.length; ++i) {
    const info = gl.getActiveUniform(program, indices[i]);
    measured[info.name.replace(/^.*\./, '').replace(/\[0\]$/, '')] = offsets[i];
  }
  report.programBlockLayout = {blockSize, activeUniforms, offsets: measured};
  for (const [field, offset] of Object.entries(contract.offsets)) {
    require(measured[field] === offset,
      `Program offset for ${field} is ${measured[field]}, contract says ${offset}`);
  }
  require(blockSize === contract.size,
    `Program block size ${blockSize} differs from contract ${contract.size}`);

  const binding = 0;
  gl.uniformBlockBinding(program, blockIndex, binding);
  const uniform = gl.createBuffer();
  gl.bindBufferBase(gl.UNIFORM_BUFFER, binding, uniform);

  const vao = gl.createVertexArray(); // Fullscreen triangle uses gl_VertexID; no attributes.
  gl.bindVertexArray(vao);
  gl.useProgram(program);
  gl.disable(gl.CULL_FACE);
  gl.disable(gl.DEPTH_TEST);

  // WebGL 2 default/renderbuffer sampling: linear UNORM and SRGB8_ALPHA8.
  for (const encoding of ['unorm', 'srgb']) {
    const srgb = encoding === 'srgb';
    for (const [width, height] of [[96, 64], [64, 96]]) {
      const color = gl.createRenderbuffer();
      gl.bindRenderbuffer(gl.RENDERBUFFER, color);
      gl.renderbufferStorage(gl.RENDERBUFFER, srgb ? gl.SRGB8_ALPHA8 : gl.RGBA8, width, height);
      const framebuffer = gl.createFramebuffer();
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
      gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, color);
      require(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'Framebuffer incomplete');
      gl.viewport(0, 0, width, height);
      try {
        for (const elapsed of [0, 2]) {
          upload(gl, uniform, contract, width, height, elapsed);
          gl.clearColor(1, 0, 1, 1);
          gl.clear(gl.COLOR_BUFFER_BIT);
          gl.drawArrays(gl.TRIANGLES, 0, 3);
          const pixels = new Uint8Array(width * height * 4);
          gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
          const glError = gl.getError();
          require(glError === gl.NO_ERROR, 'GL error 0x' + glError.toString(16));
          let mismatches = 0;
          let maxError = 0;
          // WebGL: both gl_FragCoord and readPixels use a bottom-left origin,
          // so the readback row equals the fragment row (no flip, unlike WebGPU).
          for (let y = 0; y < height; ++y) {
            for (let x = 0; x < width; ++x) {
              const fy = y; // fragment-space row == readback row (bottom-left origin)
              const cx = (x + 0.5 - width / 2) / height;
              const cy = (fy + 0.5 - height / 2) / height;
              const circle = cx * cx + cy * cy < 0.04 ? 1 : 0;
              const expected = [
                0.6 * (x + 0.5) / width + 0.2 * elapsed * 0.05,
                0.4 * (fy + 0.5) / height + 0.4 * elapsed * 0.05,
                0.2 * circle + 0.6 * elapsed * 0.05,
                1,
              ];
              expected.forEach((value, c) => {
                const error = Math.abs(pixels[(y * width + x) * 4 + c] - encode(value, srgb && c < 3));
                maxError = Math.max(maxError, error);
                if (error > 2) ++mismatches;
              });
            }
          }
          report.cases.push({encoding, width, height, elapsed, mismatches, maxError});
          require(mismatches === 0, 'Pixel mismatch: ' + JSON.stringify(report.cases.at(-1)));
        }
      } finally {
        gl.deleteFramebuffer(framebuffer);
        gl.deleteRenderbuffer(color);
      }
    }
  }

  require(report.errors.length === 0, 'Uncaptured error');
  report.state = 'passed';
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  function frame(time) {
    if (report.state !== 'passed') {
      status.textContent = JSON.stringify(report, null, 2);
      return;
    }
    gl.viewport(0, 0, canvas.width, canvas.height);
    upload(gl, uniform, contract, canvas.width, canvas.height, (time / 1000) % 6);
    gl.clearColor(1, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    ++report.frames;
    status.textContent = JSON.stringify(report, null, 2);
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}
start().catch(error => {
  report.state = 'failed';
  report.errors.push(String(error));
  status.textContent = JSON.stringify(report, null, 2);
});
