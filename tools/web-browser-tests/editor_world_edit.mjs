// Qualification of one shared World edit through Qt/Wasm, the viewport and
// folder persistence. The existing viewport runner owns browser launch, import,
// navigation and GPU diagnostics. This module supplies its authoring workload.
import assert from 'node:assert/strict';
import {resolve} from 'node:path';
import {PNG} from 'pngjs';

const worldFile = 'gpu.world.json';
const recoveryFile = worldFile + '.ludus-recovery';
const viewport = {width: 1440, height: 1000};
const timeLimit = 30000;
const firstEdit = 1.25;
const secondEdit = -1.25;
const resizedEdit = 1.5;
const finalEdit = 1.125;

// Extend the World that SceneCook actually imported. PBRT import batches
// placements while World instances intentionally keep selection and placement
// separate. Every added instance references the same cooked mesh and material.
function repeatPlacements(world, placementCount) {
  const [marker] = world.entities.filter(entity => entity.scene);
  assert.ok(marker, 'The imported World has a scene placement');
  marker.name = 'Qualification marker';
  for (let index = 1; index < placementCount; ++index) {
    const placed = structuredClone(marker);
    const suffix = String(index).padStart(12, '0');
    if (placed.uuid) {
      placed.uuid = '00000000-0000-4000-8000-' + suffix;
      placed.label = 'qualification-placement-' + index;
    } else {
      placed.id = '00000000-0000-4000-8000-' + suffix;
    }
    placed.name = 'Repeated placement ' + index;
    const column = (index - 1) % 8;
    const row = Math.floor((index - 1) / 8);
    placed.scene.position = [(column - 3.5) * 0.36, 0.62 + row * 0.15, 0];
    placed.scene.scale = [0.055, 0.055, 0.055];
    world.entities.push(placed);
  }
  return marker.id || marker.uuid;
}

async function observeEditor(page) {
  // Qt's loader already returns the running module. Read its real recovery
  // checkpoints without adding a Wasm test command or bypassing the editor UI.
  await page.route('**/editor.js', async route => {
    const response = await route.fetch();
    const loader = await response.text();
    assert.ok(loader.includes('await qtLoad({'), 'The editor loader entry point changed');
    await route.fulfill({response, body: loader.replace('await qtLoad({', 'globalThis.qaEditor = await qtLoad({')});
  });
  await page.addInitScript(({worldFile, recoveryFile}) => {
    const createWritable = FileSystemFileHandle.prototype.createWritable;
    window.qaWorld = {isWriteRefused: false, refusals: [], accepted: [], frames: [], revisions: [], drawable: [], worldCommits: 0};
    const trace = window.qaWorld;
    trace.gpu = {uploadBytes: 0, uploads: 0, draws: 0};
    function observe(prototype, method, record, isViewport = () => true) {
      const original = prototype?.[method];
      if (!original) return;
      prototype[method] = function (...parameters) {
        const result = original.apply(this, parameters);
        if (isViewport(this)) record(parameters);
        return result;
      };
    }
    function upload(data, offset = 0, length) {
      if (!data || typeof data.byteLength !== 'number') return;
      const elementBytes = data.BYTES_PER_ELEMENT || 1;
      trace.gpu.uploadBytes += length === undefined || length === 0
        ? data.byteLength - offset * elementBytes : length * elementBytes;
      ++trace.gpu.uploads;
    }
    observe(globalThis.GPUQueue?.prototype, 'writeBuffer', ([, , data, offset, length]) => upload(data, offset, length));
    observe(globalThis.GPUQueue?.prototype, 'writeTexture', ([, data]) => upload(data));
    const gl = globalThis.WebGL2RenderingContext?.prototype;
    const isViewport = context => context.canvas.classList?.contains('ludus-gpu-viewport');
    observe(gl, 'bufferData', ([, data, , offset, length]) => upload(data, offset, length), isViewport);
    observe(gl, 'bufferSubData', ([, , data, offset, length]) => upload(data, offset, length), isViewport);
    for (const method of ['drawArrays', 'drawElements', 'drawArraysInstanced', 'drawElementsInstanced']) {
      observe(gl, method, () => ++trace.gpu.draws, isViewport);
    }
    for (const method of ['draw', 'drawIndexed']) {
      observe(globalThis.GPURenderPassEncoder?.prototype, method, () => ++trace.gpu.draws);
    }
    FileSystemFileHandle.prototype.createWritable = function (...parameters) {
      // Hold one recovery checkpoint write open: the editor stays suspended in
      // its folder call until the test releases it.
      if (this.name === recoveryFile && trace.shouldHoldRecoveryWrite) {
        trace.shouldHoldRecoveryWrite = false;
        return new Promise(resolve => {
          trace.releaseRecoveryWrite = () => resolve(createWritable.apply(this, parameters));
          trace.heldRecoveryWrite = performance.now();
        });
      }
      if (this.name === worldFile && trace.isWriteRefused) {
        trace.refusals.push(performance.now());
        return Promise.reject(new DOMException('Qualification test refused this World write', 'NotAllowedError'));
      }
      const pending = createWritable.apply(this, parameters);
      if (this.name !== worldFile) return pending;
      return pending.then(stream => {
        const close = stream.close.bind(stream);
        stream.close = async () => {
          await close();
          ++trace.worldCommits;
        };
        return stream;
      });
    };
    addEventListener('ludusworldeditaccepted', event => { trace.coreAcceptedAt = event.detail; });
    addEventListener('ludusworldplacement', event => trace.revisions.push(event.detail));
    addEventListener('ludusdrawablescene', event => {
      trace.drawable.push({...event.detail, canvas: event.target.id, frame: Number(event.target.dataset.frames)});
    });
    addEventListener('DOMContentLoaded', () => {
      // A new tab's about:blank document also runs this script; only the editor page has a screen.
      const screen = document.querySelector('#screen');
      if (screen === null) return;
      screen.dataset.measureWorldEdits = 'true';
      const observer = new MutationObserver(records => {
        for (const record of records) {
          if (record.attributeName === 'data-editor-state' && document.body.dataset.editorState === 'ready') {
            trace.editorReady = performance.now();
          }
          if (record.attributeName === 'data-frames' && record.target.style.display === 'block' &&
              record.target.classList.contains('ludus-gpu-viewport')) {
            const frame = Number(record.target.dataset.frames);
            const previous = trace.frames.at(-1);
            const canvas = record.target.id;
            if (previous?.canvas !== canvas || previous?.frame !== frame) {
              trace.frames.push({canvas, frame, milliseconds: performance.now()});
            }
          }
        }
      });
      observer.observe(document.body, {subtree: true, attributes: true,
        attributeFilter: ['data-editor-state', 'data-frames']});
    });
    window.qaTrackRecovery = path => {
      const fs = window.qaEditor.FS;
      const rename = fs.rename;
      fs.rename = function (oldPath, newPath) {
        const result = rename.call(this, oldPath, newPath);
        if (newPath === path + '/' + recoveryFile) {
          const text = fs.readFile(newPath, {encoding: 'utf8'});
          const bytes = text.slice(text.indexOf('{'));
          trace.accepted.push({milliseconds: performance.now(), world: JSON.parse(bytes),
            coreAcceptedAt: trace.coreAcceptedAt, revision: trace.revisions.at(-1)});
        }
        return result;
      };
    };
  }, {worldFile, recoveryFile});
}

async function openProject(page, folderName, shouldSeed) {
  await page.evaluate(async ({folderName, shouldSeed}) => {
    const root = await navigator.storage.getDirectory();
    const folder = await root.getDirectoryHandle(folderName, {create: shouldSeed});
    if (shouldSeed) {
      const handle = await folder.getFileHandle('ludus.project.json', {create: true});
      const writable = await handle.createWritable();
      await writable.write(JSON.stringify({version: 1, name: folderName, provider: 'cmake',
        source_dir: '.', preset: 'linux-clang-development', target: 'fixture', run: {cwd: '.', args: []}}));
      await writable.close();
    }
    window.qaWorld.folder = folder;
    window.showDirectoryPicker = async () => folder;
  }, {folderName, shouldSeed});
}

async function readSavedWorld(page) {
  return page.evaluate(async name => {
    try {
      return await (await (await window.qaWorld.folder.getFileHandle(name)).getFile()).text();
    } catch (error) {
      if (error.name === 'NotFoundError') return null;
      throw error;
    }
  }, worldFile);
}

function getPlacement(world, id) {
  const object = world.entities.find(entity => entity.id === id || entity.uuid === id);
  assert.ok(object?.scene, `World is missing imported placement ${id}`);
  return object.scene.position;
}

async function waitForCheckpoint(page, id, x, after) {
  await page.waitForFunction(({id, x, after}) => window.qaWorld.accepted.some(({world, milliseconds}) =>
    milliseconds >= after && world.entities.some(entity => (entity.id === id || entity.uuid === id) && entity.scene?.position[0] === x)),
  {id, x, after}, {timeout: timeLimit});
  return page.evaluate(({id, x, after}) => window.qaWorld.accepted.find(({world, milliseconds}) =>
    milliseconds >= after && world.entities.some(entity => (entity.id === id || entity.uuid === id) && entity.scene?.position[0] === x)),
  {id, x, after});
}

async function readMarker(page, surface, predicate) {
  const box = await surface.boundingBox();
  assert.ok(box?.width > 0 && box.height > 0, 'the World viewport is visible');
  const deadline = Date.now() + timeLimit;
  do {
    const pixels = PNG.sync.read(await page.screenshot({clip: box}));
    // At the fixture camera's horizon the origin marker is the only mesh.
    // Read neutral Solid pixels, excluding axes, backdrop and selection tint.
    const row = Math.floor(pixels.height / 2) + 25;
    let count = 0;
    let sum = 0;
    for (let x = 0; x < pixels.width; ++x) {
      const offset = (row * pixels.width + x) * 4;
      const [red, green, blue] = pixels.data.subarray(offset, offset + 3);
      if (red > 100 && Math.abs(red - green) <= 2 && Math.abs(red - blue) <= 2) {
        ++count;
        sum += x;
      }
    }
    if (count > 25 && predicate(sum / count - pixels.width / 2)) {
      return page.evaluate(offsetPixels => {
        const frame = window.qaWorld.frames.at(-1);
        return {offsetPixels, observedAtMilliseconds: performance.now(), frame};
      }, sum / count - pixels.width / 2);
    }
  } while (Date.now() < deadline);
  const diagnostic = await page.locator('#screen').getAttribute('data-viewport-error');
  assert.fail('The viewport did not draw the requested placement marker' + (diagnostic ? ': ' + diagnostic : ''));
}

// Where a failed step stopped: the latest published and drawn extractions, the
// viewport canvases, and whether Qt's event loop still runs. Qt 6.10's private
// suspend/resume control holds the browser events that Qt has not sent yet; a
// control with neither a waiting event loop nor sent events no longer runs.
async function describeStall(page) {
  return page.evaluate(() => {
    const control = window.qaEditor?.qtSuspendResumeControl;
    return {
      revisions: window.qaWorld.revisions.slice(-6),
      drawable: window.qaWorld.drawable.slice(-6),
      viewports: [...document.querySelectorAll('.ludus-gpu-viewport')].map(canvas => ({
        id: canvas.id, display: canvas.style.display, frames: canvas.dataset.frames, api: canvas.dataset.api,
      })),
      qtEvents: control ? {isWaiting: control.resume != null, unsent: control.pendingEvents?.length} : null,
      status: document.querySelector('#screen')?.dataset.viewportPrompt,
    };
  });
}

async function replaceField(page, x) {
  await page.mouse.click(1280, 429);
  await page.keyboard.press('Home');
  await page.keyboard.press('Shift+End');
  await page.keyboard.type(String(x));
}

async function applyField(page) {
  await page.waitForFunction(() => document.querySelector('#screen').dataset.worldApplyEnabled === 'true',
    null, {timeout: timeLimit});
  // Capture the real browser input before Qt processes the action.
  await page.evaluate(() => {
    document.addEventListener('pointerdown', event => {
      if (event.isTrusted) window.qaWorld.input = performance.now();
    }, {capture: true, once: true});
  });
  await page.mouse.click(1350, 880);
  return page.evaluate(() => window.qaWorld.input);
}

async function waitForSavedPlacement(page, id, x) {
  await page.waitForFunction(async ({worldFile, id, x}) => {
    try {
      const handle = await window.qaWorld.folder.getFileHandle(worldFile);
      const world = JSON.parse(await (await handle.getFile()).text());
      return id === null || world.entities.some(entity => (entity.id === id || entity.uuid === id) &&
        entity.scene?.position[0] === x);
    } catch (error) {
      if (error.name === 'NotFoundError') return false;
      throw error;
    }
  }, {worldFile, id, x}, {timeout: timeLimit});
}

export async function qualifyWorldEdits({browser, canvas, click, importScene, open, rendered, output, report}) {
  report.worldEditQualification = {frontend: 'Qt/Wasm', samples: [],
    boundaries: {
      input: 'Capturing DOM pointerdown for the trusted Apply Changes click',
      accepted: 'World Apply published the validated draft, corroborated by the matching recovery checkpoint',
      drawable: 'First ready browser viewport lease carrying the published World extraction identity',
      pixels: 'First composited screenshot with the expected marker position (observation upper bound)',
      geometry: 'Authored positions and indices of the one shared triangle, before cooking',
      gpu: 'Bytes passed through WebGPU queue writes or WebGL buffer uploads, and direct draw API calls',
      unobserved: ['renderer snapshot or delta bytes', 'GPU memory traffic', 'CPU picking time'],
    }};
  const qualification = report.worldEditQualification;
  // Preserve the existing pixel-suite control on platforms that cannot open
  // disk folders. This qualification itself requires Chromium's OPFS API.
  for (const {placementCount, query, api, shouldSimulateMissingWebGpu} of [
    {placementCount: 9, query: '', api: 'webgpu'},
    {placementCount: 49, query: '', api: 'webgpu'},
    {placementCount: 9, query: '', api: 'webgl2', shouldSimulateMissingWebGpu: true},
    {placementCount: 49, query: '?viewport-backend=webgl2', api: 'webgl2'},
  ]) {
    const context = await browser.newContext({viewport});
    const folderName = 'World Qualification';
    const mount = '/browser-folders/1/' + folderName;
    const sample = {api, startupMode: shouldSimulateMissingWebGpu ? 'automatic fallback' : query ? 'forced' : 'default',
      placementCount, authoredUniqueMeshBytes: 3 * 3 * 4 + 3 * 4, edits: []};
    qualification.samples.push(sample);
    try {
      async function configurePage(page) {
        await observeEditor(page);
        if (shouldSimulateMissingWebGpu) {
          await page.addInitScript(() => Object.defineProperty(navigator, 'gpu', {value: undefined}));
        }
      }
      async function mountProject(page, shouldSeed) {
        await openProject(page, folderName, shouldSeed);
        await click(page, 54, 157);
        await page.waitForFunction(path => window.qaEditor?.FS?.analyzePath(path).exists, mount + '/ludus.project.json');
      }
      async function openSavedProject() {
        const page = await context.newPage();
        await configurePage(page);
        await open(page, query, {shouldCreateWorld: false});
        await mountProject(page, false);
        const opening = await page.evaluate(() => performance.now());
        await click(page, 100, 301); // Select the Worlds group.
        await page.keyboard.press('ArrowRight'); // Expand it if the project restored it collapsed.
        await page.mouse.dblclick(101, 322, {delay: 50});
        await rendered(page);
        assert.equal(await canvas(page).getAttribute('data-api'), api);
        return {page, opening};
      }

      const seed = await context.newPage();
      await configurePage(seed);
      await open(seed, query, {beforeNewWorld: () => mountProject(seed, true)});
      await importScene(seed);
      await rendered(seed);
      await click(seed, 142, 157); // Save the seed World.
      await waitForSavedPlacement(seed, null, 0);
      const world = JSON.parse(await readSavedWorld(seed));
      const id = repeatPlacements(world, placementCount);
      await seed.evaluate(async ({worldFile, world}) => {
        const handle = await window.qaWorld.folder.getFileHandle(worldFile);
        const stream = await handle.createWritable();
        await stream.write(JSON.stringify(world));
        await stream.close();
      }, {worldFile, world});
      const saved = await readSavedWorld(seed);
      await seed.close();
      const {page, opening} = await openSavedProject();
      const placements = world.entities.filter(entity => entity.scene);
      assert.equal(placements.length, placementCount);
      assert.equal(new Set(placements.map(entity => entity.scene.mesh)).size, 1);
      assert.equal(new Set(placements.map(entity => entity.scene.material)).size, 1);
      const marker = placements.find(entity => entity.scene.position.every(value => value === 0));
      assert.ok(marker, 'The World has an origin marker');
      sample.marker = {id, name: marker.name};
      const surface = canvas(page);
      const initial = await readMarker(page, surface, offset => Math.abs(offset) < 12);
      sample.coldWorldOpenToObservedFrameMilliseconds = initial.observedAtMilliseconds - opening;
      sample.initialGpu = await page.evaluate(() => window.qaWorld.gpu);
      await page.evaluate(path => window.qaTrackRecovery(path), mount);
      const box = await surface.boundingBox();
      const pick = await page.evaluate(() => performance.now());
      await page.mouse.click(box.x + box.width / 2 + 18, box.y + box.height / 2 - 16);
      // The production selection overlay is the viewport's picking feedback.
      await page.locator('.ludus-gpu-viewport-overlay:visible').last().waitFor({timeout: timeLimit});
      sample.pickObservedMilliseconds = await page.evaluate(start => performance.now() - start, pick);
      sample.viewport = await surface.evaluate(canvas => ({
        width: canvas.width, height: canvas.height, devicePixelRatio,
      }));
      const session = await context.newCDPSession(page);
      try {
        const {nodes} = await session.send('Accessibility.getFullAXTree');
        sample.accessibilityTree = nodes.filter(node => !node.ignored && ['tree', 'treeitem', 'table', 'textbox']
          .includes(node.role?.value)).map(node => ({role: node.role.value, name: node.name?.value}));
      } finally {
        await session.detach();
      }

      async function edit(x, predicate, {shouldResize = false, isTyped = false} = {}) {
        const gpuBefore = await page.evaluate(() => window.qaWorld.gpu);
        if (!isTyped) {
          await replaceField(page, x);
        }
        if (shouldResize) {
          const narrow = {width: 1200, height: viewport.height};
          await page.setViewportSize(narrow);
          await page.screenshot({path: resolve(output, `draft-narrow-${api}-${placementCount}.png`)});
          await page.setViewportSize(viewport);
          await page.screenshot({path: resolve(output, `draft-restored-${api}-${placementCount}.png`)});
          sample.draftWidths = [viewport.width, narrow.width, viewport.width];
        }
        const input = await applyField(page);
        const checkpoint = await waitForCheckpoint(page, id, x, input);
        assert.ok(checkpoint.coreAcceptedAt >= input && checkpoint.milliseconds >= checkpoint.coreAcceptedAt,
          'Apply must precede core publication and its recovery checkpoint');
        assert.ok(checkpoint.revision?.identity && checkpoint.revision.milliseconds >= input,
          'The checkpoint must identify the extraction accepted after this input');
        await page.waitForFunction(revision => window.qaWorld.drawable.some(frame =>
          frame.identity === revision.identity && frame.milliseconds >= revision.milliseconds),
        checkpoint.revision, {timeout: timeLimit});
        const drawable = await page.evaluate(checkpoint => window.qaWorld.drawable.find(frame =>
          frame.identity === checkpoint.revision.identity && frame.milliseconds >= checkpoint.revision.milliseconds),
        checkpoint);
        const frame = await readMarker(page, surface, predicate);
        for (const original of placements) {
          const originalId = original.id || original.uuid;
          assert.deepEqual(getPlacement(checkpoint.world, originalId),
            originalId === id ? [x, 0, 0] : original.scene.position);
        }
        const gpuAfter = await page.evaluate(() => window.qaWorld.gpu);
        const gpu = Object.fromEntries(Object.keys(gpuBefore).map(key => [key, gpuAfter[key] - gpuBefore[key]]));
        sample.edits.push({x, gpu, inputToCoreAcceptanceMilliseconds: checkpoint.coreAcceptedAt - input,
          coreAcceptanceToDrawableMilliseconds: drawable.milliseconds - checkpoint.coreAcceptedAt,
          publicationToDrawableMilliseconds: drawable.milliseconds - checkpoint.revision.milliseconds,
          checkpointToObservedFrameMilliseconds: frame.observedAtMilliseconds - checkpoint.milliseconds,
          revision: checkpoint.revision.identity, drawable, ...frame});
      }
      await edit(firstEdit, offset => offset > initial.offsetPixels + 70);
      await edit(secondEdit, offset => offset < initial.offsetPixels - 70);
      await edit(resizedEdit, offset => offset > initial.offsetPixels + 90, {shouldResize: true});
      // Two distinct accepted edits with no frame wait between them. After the
      // latest revision becomes drawable, no earlier revision may replace it.
      // The later edit is typed while the earlier edit's recovery checkpoint is
      // still being written to the folder, so the editor is suspended and Qt
      // queues the click and keys: none may be lost or applied out of order.
      await page.evaluate(() => { window.qaWorld.shouldHoldRecoveryWrite = true; });
      await replaceField(page, -0.75);
      const earlierInput = await applyField(page);
      const earlier = await waitForCheckpoint(page, id, -0.75, earlierInput);
      await page.waitForFunction(() => window.qaWorld.heldRecoveryWrite !== undefined, null, {timeout: timeLimit});
      await replaceField(page, finalEdit);
      sample.typedDuringFolderWrite = await page.evaluate(() => ({
        unsentQtEvents: window.qaEditor.qtSuspendResumeControl.pendingEvents.length,
        focus: document.activeElement === document.body ? 'body' : document.activeElement?.id || document.activeElement?.tagName,
      }));
      await page.evaluate(() => window.qaWorld.releaseRecoveryWrite());
      assert.ok(sample.typedDuringFolderWrite.unsentQtEvents > 0,
        'The final edit must be typed while the folder write keeps the editor suspended');
      assert.notEqual(sample.typedDuringFolderWrite.focus, 'body',
        "A click queued during a folder write must not move focus out of Qt's window");
      await edit(finalEdit, offset => offset > initial.offsetPixels + 120 && offset < initial.offsetPixels + 200,
        {isTyped: true});
      const latest = sample.edits.at(-1).drawable;
      const laterRevisions = await page.evaluate(time => window.qaWorld.drawable
        .filter(frame => frame.milliseconds >= time).map(frame => frame.identity), latest.milliseconds);
      assert.ok(earlier.revision?.identity);
      assert.notEqual(earlier.revision.identity, latest.identity);
      assert.ok(!laterRevisions.includes(earlier.revision.identity), 'A superseded placement drew after the latest one');
      sample.rapidEdits = {earlier: earlier.revision.identity, latest: latest.identity};

      await page.evaluate(() => { window.qaWorld.isWriteRefused = true; });
      await click(page, 142, 157); // Save World.
      await page.waitForFunction(() => window.qaWorld.refusals.length > 0, null, {timeout: timeLimit});
      assert.equal(await readSavedWorld(page), saved, 'The refused write must preserve the accepted project bytes');
      assert.equal(await page.evaluate(() => window.qaWorld.worldCommits), 0);
      const sessionCopy = await page.evaluate(path => window.qaEditor.FS.readFile(path, {encoding: 'utf8'}),
        mount + '/' + worldFile);
      assert.deepEqual(getPlacement(JSON.parse(sessionCopy), id), [finalEdit, 0, 0],
        'The refused write must retain the pending placement in the tab');
      const warnings = page.getByText('Browser folders', {exact: true});
      await warnings.last().waitFor({timeout: timeLimit});
      const refusalCount = await page.evaluate(() => window.qaWorld.refusals.length);
      sample.recovery = {refusedWrites: refusalCount, dismissedWarnings: 0, reopened: false};
      await page.screenshot({path: resolve(output, `save-refused-${api}-${placementCount}.png`)});
      while (await warnings.count() > 0 && sample.recovery.dismissedWarnings < refusalCount) {
        await click(page, 930, 586); // OK in the centered Browser folders warning.
        ++sample.recovery.dismissedWarnings;
      }
      await warnings.last().waitFor({state: 'hidden', timeout: timeLimit});
      await readMarker(page, surface, offset => offset > initial.offsetPixels + 120 && offset < initial.offsetPixels + 200);
      await page.evaluate(() => { window.qaWorld.isWriteRefused = false; });
      await click(page, 142, 157); // Save World.
      await waitForSavedPlacement(page, id, finalEdit);
      const recovered = JSON.parse(await readSavedWorld(page));
      for (const original of placements) {
        const originalId = original.id || original.uuid;
        assert.deepEqual(getPlacement(recovered, originalId), originalId === id ? [finalEdit, 0, 0] : original.scene.position);
      }

      await page.waitForFunction(() => window.ludusHostPendingWrites?.() === 0, null, {timeout: timeLimit});
      assert.equal(await page.evaluate(path => window.qaEditor.FS.analyzePath(path).exists,
        mount + '/' + recoveryFile), false, 'A successful retry must acknowledge the World and clear recovery');
      sample.recovery.worldCommits = await page.evaluate(() => window.qaWorld.worldCommits);
      assert.equal(sample.recovery.worldCommits, 1, 'Recovery must publish the intended World once');
      await page.close();

      const {page: reopened, opening: reopening} = await openSavedProject();
      const reopenedFrame = await readMarker(reopened, canvas(reopened),
        offset => offset > initial.offsetPixels + 120 && offset < initial.offsetPixels + 200);
      sample.reopenToObservedFrameMilliseconds = reopenedFrame.observedAtMilliseconds - reopening;
      sample.startup = await reopened.evaluate(() => ({
        editorReadyMilliseconds: window.qaWorld.editorReady,
        encodedAssets: performance.getEntriesByType('resource').filter(entry => /\.(js|wasm|css)$/.test(entry.name))
          .map(entry => ({file: new URL(entry.name).pathname.split('/').at(-1), encodedBytes: entry.encodedBodySize})),
        devicePixelRatio,
        wasmHeapBytes: window.qaEditor.HEAPU8?.byteLength ?? null,
        userAgent: navigator.userAgent,
      }));
      const orbitBox = await canvas(reopened).boundingBox();
      const orbitStart = await reopened.evaluate(() => window.qaWorld.frames.length);
      await reopened.keyboard.down('Alt');
      try {
        await reopened.mouse.move(orbitBox.x + orbitBox.width / 2, orbitBox.y + orbitBox.height / 2);
        await reopened.mouse.down();
        await reopened.mouse.move(orbitBox.x + orbitBox.width / 2 + 80, orbitBox.y + orbitBox.height / 2 + 30,
          {steps: 16});
        await reopened.mouse.up();
      } finally {
        await reopened.keyboard.up('Alt');
      }
      sample.orbitFrameIntervalsMilliseconds = await reopened.evaluate(start => {
        const frames = window.qaWorld.frames.slice(start);
        return frames.slice(1).map((frame, index) => frame.milliseconds - frames[index].milliseconds);
      }, orbitStart);
      sample.recovery.reopened = true;
    } catch (error) {
      const active = context.pages().at(-1);
      if (active && !active.isClosed()) {
        await active.screenshot({path: resolve(output, `qualification-failure-${api}-${placementCount}.png`)});
        sample.failureState = await describeStall(active).catch(reason => ({unavailable: String(reason)}));
      }
      throw error;
    } finally {
      await context.close();
    }
  }
  report.cases.push('Shared repeated-placement World edits and refused-save recovery on WebGPU and WebGL 2');
}
