// Tools-only consumer: the installed Ludus RHI has no public resource API yet.
const status = document.querySelector('#status');
const report = window.shaderProbe = {state:'pending', cases:[], errors:[], frames:0};
function require(condition, message) {if (!condition) throw new Error(message);}
function encode(value, srgb) {
  return Math.round(255 * (srgb ? (value <= 0.0031308 ? 12.92 * value :
    1.055 * Math.pow(value, 1/2.4) - 0.055) : value));
}
function upload(device, buffer, contract, width, height, elapsed) {
  const bytes = new ArrayBuffer(contract.size);
  const view = new DataView(bytes);
  // Byte offsets are independently checked from emitted WGSL by shader-probe.
  const values = {Resolution:[width,height], ElapsedTime:[elapsed],
    Direction:[0.2,0.4,0.6], Tint:[0.6,0.4,0.2,1]};
  for (const [field, data] of Object.entries(values)) {
    data.forEach((value,i) => view.setFloat32(contract.offsets[field] + 4*i,value,true));
  }
  device.queue.writeBuffer(buffer,0,bytes);
}
async function start() {
  require(navigator.gpu, 'WebGPU unavailable');
  const adapter = await navigator.gpu.requestAdapter();
  require(adapter, 'No WebGPU adapter');
  report.adapter = {vendor:adapter.info.vendor, architecture:adapter.info.architecture,
    device:adapter.info.device, description:adapter.info.description};
  const device = await adapter.requestDevice();
  device.addEventListener('uncapturederror', e => {
    report.errors.push(e.error.message); report.state='failed';
  });
  device.lost.then(info => {report.state='failed'; report.errors.push('Device lost: '+info.message);});
  const shaderResponse = await fetch('probe.wgsl');
  require(shaderResponse.ok, 'Cannot load generated WGSL');
  const code = await shaderResponse.text();
  const contractResponse = await fetch('contract.json');
  require(contractResponse.ok, 'Cannot load layout contract');
  const contract = (await contractResponse.json()).wgsl;
  require(contract.size===48 && contract.alignment===16, 'Unexpected upload contract');
  device.pushErrorScope('validation');
  const module = device.createShaderModule({code});
  report.compilationMessages = (await module.getCompilationInfo()).messages.map(m =>
    ({type:m.type, message:m.message, line:m.lineNum}));
  require(!report.compilationMessages.some(m => m.type==='error'), 'WGSL compilation failed');
  const bindings = device.createBindGroupLayout({entries:[{binding:0,visibility:GPUShaderStage.FRAGMENT,
    buffer:{type:'uniform', minBindingSize:48}}]});
  const layout = device.createPipelineLayout({bindGroupLayouts:[bindings]});
  const uniform = device.createBuffer({size:48,usage:GPUBufferUsage.UNIFORM|GPUBufferUsage.COPY_DST});
  const group = device.createBindGroup({layout:bindings,entries:[{binding:0,resource:{buffer:uniform,size:48}}]});
  async function pipeline(format) {
    return device.createRenderPipelineAsync({layout,vertex:{module,entryPoint:'VertexMain'},
      fragment:{module,entryPoint:'FragmentMain',targets:[{format}]},
      primitive:{topology:'triangle-list',cullMode:'none'}});
  }
  function draw(target, renderPipeline) {
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({colorAttachments:[{view:target,
      loadOp:'clear',storeOp:'store',clearValue:[1,0,1,1]}]});
    pass.setPipeline(renderPipeline); pass.setBindGroup(0,group); pass.draw(3); pass.end();
    return encoder;
  }
  for (const format of ['rgba8unorm','rgba8unorm-srgb']) {
    const renderPipeline = await pipeline(format);
    for (const [width,height] of [[96,64],[64,96]]) {
      const target = device.createTexture({size:[width,height],format,
        usage:GPUTextureUsage.RENDER_ATTACHMENT|GPUTextureUsage.COPY_SRC});
      const stride = Math.ceil(width*4/256)*256;
      const readback = device.createBuffer({size:stride*height,
        usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
      try {
        for (const elapsed of [0,2]) {
          upload(device,uniform,contract,width,height,elapsed);
          const encoder = draw(target.createView(),renderPipeline);
          encoder.copyTextureToBuffer({texture:target},{buffer:readback,bytesPerRow:stride},[width,height]);
          device.queue.submit([encoder.finish()]);
          await readback.mapAsync(GPUMapMode.READ);
          let mismatches=0, maxError=0;
          const pixels = new Uint8Array(readback.getMappedRange());
          for (let y=0;y<height;++y) for (let x=0;x<width;++x) {
            const cx=(x+0.5-width/2)/height, cy=(y+0.5-height/2)/height;
            const circle=cx*cx+cy*cy<0.04 ? 1 : 0;
            const expected=[0.6*(x+0.5)/width+0.01*elapsed,
              0.4*(y+0.5)/height+0.02*elapsed, 0.2*circle+0.03*elapsed,1];
            expected.forEach((value,c) => {
              const error = Math.abs(pixels[y*stride+x*4+c]-encode(value,format.endsWith('srgb')&&c<3));
              maxError=Math.max(maxError,error);
              if (error>2) ++mismatches;
            });
          }
          readback.unmap();
          report.cases.push({format,width,height,elapsed,mismatches,maxError});
          require(mismatches===0, 'Pixel mismatch: '+JSON.stringify(report.cases.at(-1)));
        }
      } finally {readback.destroy();target.destroy();}
    }
  }
  const canvas = document.querySelector('canvas');
  const context = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({device,format,alphaMode:'opaque'});
  const display = await pipeline(format);
  const validation = await device.popErrorScope();
  require(!validation, validation?.message);
  require(report.errors.length===0, 'Uncaptured validation error');
  report.state='passed';
  report.canvasFormat=format;
  function frame(time) {
    if (report.state!=='passed') {status.textContent=JSON.stringify(report,null,2);return;}
    // Presentation runs after the bounded readback tests; no compiler/download.
    upload(device,uniform,contract,canvas.width,canvas.height,(time/1000)%6);
    device.queue.submit([draw(context.getCurrentTexture().createView(),display).finish()]);
    ++report.frames;
    status.textContent=JSON.stringify(report,null,2);
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}
start().catch(error => {report.state='failed';report.errors.push(String(error));status.textContent=JSON.stringify(report,null,2);});
