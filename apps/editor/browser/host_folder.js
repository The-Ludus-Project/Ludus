// Host project folders for the browser editor: the picked folder on disk is
// mounted into the Emscripten file system without copying it. A file's bytes are
// read on its first open(); saves are written back to the folder when the browser
// granted read-write access. See docs/architecture/editor-browser-strategy.md
// (host project folders) for the contract and its limits.
//
// Thanks to the WHATWG and WICG editors, "File System Standard" and "File System
// Access": FileSystemDirectoryHandle enumeration, FileSystemFileHandle.getFile(),
// createWritable() (an atomic swap on close in Chromium), removeEntry() and the
// queryPermission()/requestPermission() pair.
// https://fs.spec.whatwg.org/ and https://wicg.github.io/file-system-access/
// Thanks to MDN contributors, "Window: showDirectoryPicker() method" and
// "HTMLInputElement: webkitdirectory property": feature detection and the
// directory <input> fallback that Firefox and Safari implement.
// https://developer.mozilla.org/en-US/docs/Web/API/Window/showDirectoryPicker
// https://developer.mozilla.org/en-US/docs/Web/API/HTMLInputElement/webkitdirectory
// Thanks to the Emscripten authors, src/lib/libmemfs.js, libsyscall.js,
// libwasi.js and libasync.js (4.0.23): the mount type delegates node storage to
// MEMFS, and the two overridden syscalls suspend through Asyncify.handleSleep
// exactly as Emscripten's own fd_sync suspends for a mount's syncfs().
// https://github.com/emscripten-core/emscripten/tree/4.0.23/src/lib
//
// Version coupling: the overrides reimplement Emscripten's __syscall_openat and
// __syscall_renameat over the public FS API, and the mount type relies on MEMFS
// node fields (contents, usedBytes). Re-verify this file in a browser when
// config/web_toolchain.json changes the Emscripten pin.

if (ASYNCIFY != 1) {
  throw new Error('host_folder.js needs -sASYNCIFY=1: lazy reads suspend the calling open()');
}

addToLibrary({
  $LudusHost__deps: ['$FS', '$MEMFS', '$PATH', '$SYSCALLS', '$Asyncify', '$LudusSuspendedInput'],
  $LudusHost__postset: `
    // The page asks before leaving while saves are still reaching the folder.
    globalThis.ludusHostPendingWrites = () => LudusHost.countPendingWrites();
  `,
  $LudusHost: {
    // Picked folders waiting for the editor to admit and mount them.
    selections: new Map(),
    nextSelection: 1,
    // Mounted folders by mount point.
    mounts: new Map(),
    // Resident host-backed file nodes in least-recently-opened order.
    resident: new Map(),
    // Resident-byte budget, from the editor's BROWSER_FILE_BUDGET_BYTES.
    budgetBytes: 0,
    // EventTarget that receives read/write failure events for the editor.
    monitor: null,
    tables: null,
    nameDatabase: 'ludus-editor-folders',

    // --- Picking -------------------------------------------------------------

    supportsHandles() {
      return typeof globalThis.showDirectoryPicker === 'function';
    },

    dispatch(target, type, detail) {
      target.dispatchEvent(new CustomEvent(type, {detail}));
    },

    describeError(error) {
      return error?.name ? `${error.name}: ${error.message}` : String(error);
    },

    // Walk a directory handle; record file handles and sizes without reading bytes.
    async enumerate(directory, options) {
      const files = new Map();
      const directories = [];
      const pending = [[directory, '']];
      while (pending.length > 0) {
        const [handle, prefix] = pending.pop();
        let entries;
        try {
          entries = [];
          for await (const entry of handle.values()) {
            entries.push(entry);
          }
        } catch (error) {
          return {failure: 'unreadable', path: prefix || '.', error: LudusHost.describeError(error)};
        }
        for (const entry of entries) {
          const relative = prefix ? prefix + '/' + entry.name : entry.name;
          if (prefix === '' && options.skip.includes(entry.name)) {
            continue;
          }
          if (entry.kind === 'directory') {
            directories.push(relative);
            pending.push([entry, relative]);
            continue;
          }
          if (files.size >= options.maximumEntries) {
            return {failure: 'too-many', count: files.size + 1};
          }
          try {
            const file = await entry.getFile();
            files.set(relative, {source: entry, size: file.size});
          } catch (error) {
            return {failure: 'unreadable', path: relative, error: LudusHost.describeError(error)};
          }
        }
      }
      return {files, directories};
    },

    async openDatabase() {
      return await new Promise((resolve, reject) => {
        const request = indexedDB.open(LudusHost.nameDatabase, 1);
        request.onupgradeneeded = () => request.result.createObjectStore('handles');
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
    },

    async loadRemembered() {
      try {
        const database = await LudusHost.openDatabase();
        return await new Promise(resolve => {
          const request = database.transaction('handles').objectStore('handles').get('project');
          request.onsuccess = () => resolve(request.result ?? null);
          request.onerror = () => resolve(null);
        });
      } catch {
        return null;
      }
    },

    async remember(handle) {
      try {
        const database = await LudusHost.openDatabase();
        database.transaction('handles', 'readwrite').objectStore('handles').put({name: handle.name, handle}, 'project');
      } catch (error) {
        console.warn('Ludus could not remember the project folder', error);
      }
    },

    async publishHandle(target, directory, writable, intent, options) {
      const listed = await LudusHost.enumerate(directory, options);
      if (listed.failure) {
        LudusHost.dispatch(target, 'ludushostfailed', {name: directory.name, ...listed});
        return;
      }
      const id = LudusHost.nextSelection++;
      LudusHost.selections.set(id, {
        name: directory.name, directory, writable, intent, files: listed.files, directories: listed.directories,
      });
      LudusHost.dispatch(target, 'ludushostpicked', {
        selection: id,
        name: directory.name,
        writable,
        entries: [...listed.files].map(([path, file]) => ({path, size: file.size})),
      });
    },

    async pickWithHandle(target, intent, options) {
      const mode = intent === 0 ? 'readwrite' : 'read';
      const remembered = intent === 0 ? await LudusHost.loadRemembered() : null;
      let directory;
      try {
        directory = await globalThis.showDirectoryPicker({
          id: intent === 0 ? 'ludus-project' : 'ludus-scene',
          mode,
          ...(remembered ? {startIn: remembered.handle} : {}),
        });
      } catch (error) {
        if (error?.name === 'AbortError') {
          LudusHost.dispatch(target, 'ludushostcancel', {});
        } else if (error?.name === 'SecurityError' || error?.name === 'NotAllowedError') {
          LudusHost.dispatch(target, 'ludushostfailed', {failure: 'activation', error: LudusHost.describeError(error)});
        } else {
          LudusHost.dispatch(target, 'ludushostfailed', {failure: 'unreadable', path: '.', error: LudusHost.describeError(error)});
        }
        return;
      }
      // Chromium grants the requested mode with the picker; check rather than assume.
      let writable = false;
      if (intent === 0) {
        writable = (await directory.queryPermission?.({mode: 'readwrite'})) === 'granted';
        if (writable) {
          await LudusHost.remember(directory);
        }
      }
      await LudusHost.publishHandle(target, directory, writable, intent, options);
    },

    pickWithInput(target, intent, options) {
      const input = document.createElement('input');
      input.type = 'file';
      input.webkitdirectory = true;
      input.multiple = true;
      input.hidden = true;
      const finish = () => input.remove();
      input.addEventListener('cancel', () => {
        finish();
        LudusHost.dispatch(target, 'ludushostcancel', {});
      });
      input.addEventListener('change', () => {
        finish();
        const files = new Map();
        let name = '';
        for (const file of input.files) {
          const path = file.webkitRelativePath || file.name;
          const slash = path.indexOf('/');
          name ||= slash > 0 ? path.slice(0, slash) : '';
          const relative = slash > 0 ? path.slice(slash + 1) : path;
          const top = relative.split('/')[0];
          if (relative.includes('/') && options.skip.includes(top)) {
            continue;
          }
          if (files.size >= options.maximumEntries) {
            LudusHost.dispatch(target, 'ludushostfailed', {failure: 'too-many', name, count: files.size + 1});
            return;
          }
          files.set(relative, {source: file, size: file.size});
        }
        if (files.size === 0) {
          LudusHost.dispatch(target, 'ludushostcancel', {});
          return;
        }
        const id = LudusHost.nextSelection++;
        LudusHost.selections.set(id, {name, directory: null, writable: false, intent, files, directories: []});
        LudusHost.dispatch(target, 'ludushostpicked', {
          selection: id, name, writable: false,
          entries: [...files].map(([path, file]) => ({path, size: file.size})),
        });
      });
      document.body.appendChild(input);
      input.click();
    },

    async reopen(target, options) {
      const remembered = await LudusHost.loadRemembered();
      if (!remembered) {
        LudusHost.dispatch(target, 'ludushostfailed', {failure: 'no-remembered'});
        return;
      }
      let permission = 'denied';
      try {
        permission = await remembered.handle.queryPermission({mode: 'readwrite'});
        if (permission !== 'granted') {
          permission = await remembered.handle.requestPermission({mode: 'readwrite'});
        }
      } catch (error) {
        LudusHost.dispatch(target, 'ludushostfailed', {
          failure: 'permission', name: remembered.name, error: LudusHost.describeError(error),
        });
        return;
      }
      if (permission !== 'granted') {
        LudusHost.dispatch(target, 'ludushostfailed', {failure: 'permission', name: remembered.name});
        return;
      }
      await LudusHost.publishHandle(target, remembered.handle, true, 0, options);
    },

    // --- Mounting ------------------------------------------------------------

    mount(selectionId, mountPoint, budgetBytes) {
      const selection = LudusHost.selections.get(selectionId);
      if (!selection) {
        return {{{ cDefs.ENOENT }}};
      }
      LudusHost.selections.delete(selectionId);
      LudusHost.budgetBytes = budgetBytes;
      const state = {
        name: selection.name,
        mountPoint,
        rootPath: mountPoint + '/' + selection.name,
        writable: selection.writable,
        directory: selection.directory,
        skip: selection.skip ?? [],
        directoryHandles: new Map(),
        pendingNodes: new Set(),
        pendingRemovals: new Set(),
        populating: true,
        flushing: Promise.resolve(0),
        flushScheduled: false,
        flushesInFlight: 0,
      };
      try {
        FS.mkdirTree(mountPoint);
        FS.mount(LudusHost.fileSystem, {state}, mountPoint);
        FS.mkdir(state.rootPath);
        for (const directory of selection.directories) {
          FS.mkdirTree(state.rootPath + '/' + directory);
        }
        for (const [relative, file] of selection.files) {
          const path = state.rootPath + '/' + relative;
          FS.mkdirTree(PATH.dirname(path));
          const node = FS.create(path, 0o644);
          node.usedBytes = file.size;
          node.ludusHost.source = file.source;
          node.ludusHost.isResident = false;
          LudusHost.resident.delete(node);
        }
      } catch (error) {
        console.error('Ludus could not mount the host folder', error);
        return error?.errno ?? {{{ cDefs.EIO }}};
      }
      state.populating = false;
      LudusHost.mounts.set(mountPoint, state);
      return 0;
    },

    // The mount type: MEMFS nodes whose operations record changes for the folder.
    fileSystem: {
      mount(mount) {
        const root = MEMFS.mount(mount);
        LudusHost.adopt(root, mount.opts.state);
        return root;
      },
      // fsync() and directory fsync() reach here through Emscripten's fd_sync.
      syncfs(mount, populate, callback) {
        LudusHost.suspend(LudusHost.flush(mount.opts.state),
                          errno => callback(errno ? new FS.ErrnoError(errno) : null));
      },
    },

    buildTables() {
      const directory = MEMFS.ops_table.dir;
      const file = MEMFS.ops_table.file;
      const record = node => LudusHost.record(node);
      LudusHost.tables = {
        directoryNode: {
          ...directory.node,
          mknod(parent, name, mode, dev) {
            const node = MEMFS.node_ops.mknod(parent, name, mode, dev);
            LudusHost.adopt(node, parent.ludusHost.state);
            record(node);
            return node;
          },
          rename(node, newDirectory, newName) {
            const oldPath = LudusHost.relativePath(node);
            const replaced = newDirectory.contents[newName];
            MEMFS.node_ops.rename(node, newDirectory, newName);
            LudusHost.forget(replaced);
            LudusHost.recordRemoval(node.ludusHost.state, oldPath);
            LudusHost.recordTree(node);
          },
          unlink(parent, name) {
            const node = parent.contents[name];
            const path = LudusHost.relativePath(node);
            MEMFS.node_ops.unlink(parent, name);
            LudusHost.forget(node);
            LudusHost.recordRemoval(parent.ludusHost.state, path);
          },
          rmdir(parent, name) {
            const node = parent.contents[name];
            const path = LudusHost.relativePath(node);
            MEMFS.node_ops.rmdir(parent, name);
            LudusHost.recordRemoval(parent.ludusHost.state, path);
          },
          symlink() {
            // Folders picked from disk hold regular files and directories only.
            throw new FS.ErrnoError({{{ cDefs.EPERM }}});
          },
        },
        directoryStream: directory.stream,
        fileNode: {
          ...file.node,
          setattr(node, attribute) {
            if (attribute.size !== undefined) {
              if (!node.ludusHost.isResident) {
                if (attribute.size !== 0) {
                  throw new FS.ErrnoError({{{ cDefs.EIO }}});
                }
                LudusHost.makeResident(node, null);
              }
              MEMFS.node_ops.setattr(node, attribute);
              record(node);
              return;
            }
            MEMFS.node_ops.setattr(node, attribute);
          },
        },
        fileStream: {
          ...file.stream,
          open(stream) {
            const host = stream.node.ludusHost;
            if (!host.isResident) {
              throw new FS.ErrnoError({{{ cDefs.EIO }}});
            }
            host.openCount += 1;
            LudusHost.touch(stream.node);
          },
          close(stream) {
            const host = stream.node.ludusHost;
            host.openCount = Math.max(0, host.openCount - 1);
            if (host.state.pendingNodes.size > 0 || host.state.pendingRemovals.size > 0) {
              LudusHost.scheduleFlush(host.state);
            }
          },
          write(stream, buffer, offset, length, position) {
            if (!stream.node.ludusHost.isResident) {
              throw new FS.ErrnoError({{{ cDefs.EIO }}});
            }
            // Never let MEMFS alias Wasm memory: the bytes are written to disk later.
            const written = MEMFS.stream_ops.write(stream, buffer, offset, length, position, false);
            record(stream.node);
            return written;
          },
          mmap(stream, length, position, prot, flags) {
            if (!stream.node.ludusHost.isResident) {
              throw new FS.ErrnoError({{{ cDefs.EIO }}});
            }
            return MEMFS.stream_ops.mmap(stream, length, position, prot, flags);
          },
        },
      };
    },

    adopt(node, state) {
      if (!LudusHost.tables) {
        LudusHost.buildTables();
      }
      const tables = LudusHost.tables;
      if (FS.isDir(node.mode)) {
        node.node_ops = tables.directoryNode;
        node.stream_ops = tables.directoryStream;
      } else if (FS.isFile(node.mode)) {
        node.node_ops = tables.fileNode;
        node.stream_ops = tables.fileStream;
      }
      node.ludusHost = {state, source: null, isResident: true, openCount: 0};
      if (FS.isFile(node.mode)) {
        LudusHost.resident.set(node, true);
      }
    },

    relativePath(node) {
      const state = node.ludusHost.state;
      const path = FS.getPath(node);
      return path.startsWith(state.rootPath + '/') ? path.slice(state.rootPath.length + 1) : null;
    },

    isMirrored(state, relative) {
      return state.writable && relative !== null && !state.skip.includes(relative.split('/')[0]);
    },

    record(node) {
      const state = node.ludusHost.state;
      if (!state.populating) {
        state.pendingNodes.add(node);
      }
    },

    recordTree(node) {
      LudusHost.record(node);
      if (FS.isDir(node.mode)) {
        for (const child of Object.values(node.contents)) {
          LudusHost.recordTree(child);
        }
      }
    },

    recordRemoval(state, relative) {
      if (!state.populating && relative !== null) {
        state.pendingRemovals.add(relative);
      }
    },

    forget(node) {
      if (node?.ludusHost) {
        LudusHost.resident.delete(node);
        node.ludusHost.state.pendingNodes.delete(node);
      }
    },

    // --- Lazy reads and the resident budget ----------------------------------

    touch(node) {
      if (LudusHost.resident.delete(node)) {
        LudusHost.resident.set(node, true);
      }
    },

    residentBytes() {
      let total = 0;
      for (const node of LudusHost.resident.keys()) {
        total += node.contents ? node.usedBytes : 0;
      }
      return total;
    },

    // Drop clean, closed copies of files the folder can supply again.
    makeRoom(bytes) {
      let total = LudusHost.residentBytes();
      for (const node of [...LudusHost.resident.keys()]) {
        if (total + bytes <= LudusHost.budgetBytes) {
          return;
        }
        const host = node.ludusHost;
        if (host.source && host.openCount === 0 && !host.state.pendingNodes.has(node) && node.contents) {
          total -= node.usedBytes;
          node.contents = null;
          host.isResident = false;
          LudusHost.resident.delete(node);
        }
      }
    },

    makeResident(node, bytes) {
      node.contents = bytes;
      node.usedBytes = bytes ? bytes.length : 0;
      node.ludusHost.isResident = true;
      LudusHost.resident.set(node, true);
    },

    async readSource(source) {
      const file = typeof source.getFile === 'function' ? await source.getFile() : source;
      return new Uint8Array(await file.arrayBuffer());
    },

    async materialize(node, path) {
      const host = node.ludusHost;
      try {
        const file = typeof host.source.getFile === 'function' ? await host.source.getFile() : host.source;
        if (file.size > LudusHost.budgetBytes) {
          LudusHost.report('ludushostreadfailed', host.state, path, `RangeError: ${file.size} bytes exceed the budget`);
          return {{{ cDefs.EFBIG }}};
        }
        LudusHost.makeRoom(file.size);
        LudusHost.makeResident(node, new Uint8Array(await file.arrayBuffer()));
        return 0;
      } catch (error) {
        LudusHost.report('ludushostreadfailed', host.state, path, LudusHost.describeError(error));
        return {{{ cDefs.EIO }}};
      }
    },

    // A host file whose bytes the open() at `path` must read first, or null.
    lazyNodeAt(path, flags) {
      if (!path.startsWith('/') || LudusHost.mounts.size === 0) {
        return null;
      }
      let node;
      try {
        node = FS.lookupPath(path, {follow: true}).node;
      } catch {
        return null;
      }
      if (!node?.ludusHost || !FS.isFile(node.mode) || node.ludusHost.isResident) {
        return null;
      }
      const truncates = (flags & {{{ cDefs.O_TRUNC }}}) && (flags & {{{ cDefs.O_ACCMODE }}}) !== {{{ cDefs.O_RDONLY }}};
      return truncates ? null : node;
    },

    report(type, state, path, error) {
      console.warn(`Ludus host folder ${state.name}: ${type} ${path}: ${error}`);
      const monitor = LudusHost.monitor;
      if (monitor) {
        // The caller may be suspended in a file syscall. Let its promise
        // resume Wasm before Qt opens a dialog and starts handling input.
        setTimeout(() => {
          if (LudusHost.monitor === monitor) {
            LudusHost.dispatch(monitor, type, {name: state.name, path, error});
          }
        }, 0);
      }
    },

    // --- Write-back ----------------------------------------------------------

    countPendingWrites() {
      let count = 0;
      for (const state of LudusHost.mounts.values()) {
        if (state.writable) {
          count += state.pendingNodes.size + state.pendingRemovals.size + state.flushesInFlight +
            (state.flushScheduled ? 1 : 0);
        }
      }
      return count;
    },

    hasPending(state) {
      return state.writable && (state.pendingNodes.size > 0 || state.pendingRemovals.size > 0);
    },

    scheduleFlush(state) {
      if (!state.writable || state.flushScheduled) {
        return;
      }
      state.flushScheduled = true;
      setTimeout(() => {
        state.flushScheduled = false;
        LudusHost.flush(state);
      }, 0);
    },

    async directoryHandle(state, relative) {
      if (relative === '') {
        return state.directory;
      }
      const cached = state.directoryHandles.get(relative);
      if (cached) {
        return cached;
      }
      const parent = await LudusHost.directoryHandle(state, PATH.dirname(relative) === '.' ? '' : PATH.dirname(relative));
      const handle = await parent.getDirectoryHandle(PATH.basename(relative), {create: true});
      state.directoryHandles.set(relative, handle);
      return handle;
    },

    parentOf(relative) {
      const slash = relative.lastIndexOf('/');
      return slash < 0 ? '' : relative.slice(0, slash);
    },

    // Make the folder match MEMFS for every recorded change; serialized per mount.
    flush(state) {
      state.flushesInFlight += 1;
      const next = state.flushing.then(() => LudusHost.flushNow(state)).finally(() => {
        state.flushesInFlight -= 1;
      });
      state.flushing = next;
      return next;
    },

    async flushNow(state) {
      if (!LudusHost.hasPending(state)) {
        return 0;
      }
      const nodes = [...state.pendingNodes];
      const removals = [...state.pendingRemovals];
      state.pendingNodes.clear();
      state.pendingRemovals.clear();
      // Parents first, so a file's folder exists before the file.
      const writes = nodes
        .map(node => ({node, path: LudusHost.relativePath(node)}))
        .filter(write => LudusHost.isMirrored(state, write.path) && FS.getPath(write.node) === state.rootPath + '/' + write.path)
        .sort((a, b) => a.path.split('/').length - b.path.split('/').length);
      let failedPath = null;
      try {
        for (const write of writes) {
          failedPath = write.path;
          if (FS.isDir(write.node.mode)) {
            await LudusHost.directoryHandle(state, write.path);
            continue;
          }
          const host = write.node.ludusHost;
          const bytes = host.isResident
            ? MEMFS.getFileDataAsTypedArray(write.node).slice()
            : await LudusHost.readSource(host.source);
          const parent = await LudusHost.directoryHandle(state, LudusHost.parentOf(write.path));
          const handle = await parent.getFileHandle(PATH.basename(write.path), {create: true});
          const stream = await handle.createWritable({keepExistingData: false});
          await stream.write(bytes);
          await stream.close();
          host.source = handle;
        }
        for (const path of removals) {
          failedPath = path;
          if (!LudusHost.isMirrored(state, path) || LudusHost.existsInSession(state, path)) {
            continue;
          }
          for (const key of [...state.directoryHandles.keys()]) {
            if (key === path || key.startsWith(path + '/')) {
              state.directoryHandles.delete(key);
            }
          }
          const parent = await LudusHost.directoryHandle(state, LudusHost.parentOf(path));
          try {
            await parent.removeEntry(PATH.basename(path), {recursive: true});
          } catch (error) {
            if (error?.name !== 'NotFoundError') {
              throw error;
            }
          }
        }
      } catch (error) {
        // Keep every change for the next save; nothing recorded is dropped.
        for (const node of nodes) {
          state.pendingNodes.add(node);
        }
        for (const path of removals) {
          state.pendingRemovals.add(path);
        }
        LudusHost.report('ludushostwritefailed', state, failedPath ?? '.', LudusHost.describeError(error));
        return {{{ cDefs.EIO }}};
      }
      return 0;
    },

    existsInSession(state, relative) {
      try {
        FS.lookupPath(state.rootPath + '/' + relative);
        return true;
      } catch {
        return false;
      }
    },

    // --- System calls ----------------------------------------------------------

    // Emscripten 4.0.23's __syscall_openat and __syscall_renameat, unchanged.
    openAt(dirfd, path, flags, varargs) {
      try {
        const resolved = SYSCALLS.calculateAt(dirfd, SYSCALLS.getStr(path));
        const mode = varargs ? {{{ makeGetValue('varargs', 0, 'i32') }}} : 0;
        return FS.open(resolved, flags, mode).fd;
      } catch (error) {
        if (error?.name !== 'ErrnoError') throw error;
        return -error.errno;
      }
    },

    renameAt(olddirfd, oldpath, newdirfd, newpath) {
      try {
        FS.rename(SYSCALLS.calculateAt(olddirfd, SYSCALLS.getStr(oldpath)),
                  SYSCALLS.calculateAt(newdirfd, SYSCALLS.getStr(newpath)));
        return 0;
      } catch (error) {
        if (error?.name !== 'ErrnoError') throw error;
        return -error.errno;
      }
    },

    // --- Suspension guards ---------------------------------------------------

    // A nested Wasm call made while another call sleeps must not sleep itself.
    canSleep() {
      return !Asyncify.currData;
    },

    // Keep the instance suspended until `pending` settles with an errno (0 for success), then
    // resume it through `resume`. An unexpected rejection resumes with EIO rather than never.
    // Keys and Qt-window clicks meanwhile reach Qt's queue without their browser default
    // (suspended_input.js).
    suspend(pending, resume) {
      LudusSuspendedInput.begin();
      pending.catch(error => {
        console.error('Ludus host folder: a suspended file call failed', error);
        return {{{ cDefs.EIO }}};
      }).then(value => {
        try {
          resume(value);
        } finally {
          LudusSuspendedInput.end();
        }
      });
    },
  },

  // open() reads a host file's bytes the first time it is opened.
  __syscall_openat__deps: ['$LudusHost', '$SYSCALLS', '$Asyncify'],
  __syscall_openat__async: true,
  __syscall_openat: function(dirfd, path, flags, varargs) {
    let node = null;
    let resolved = '';
    if (Asyncify.state === Asyncify.State.Normal) {
      try {
        resolved = SYSCALLS.calculateAt(dirfd, SYSCALLS.getStr(path));
        node = LudusHost.lazyNodeAt(resolved, flags);
      } catch {
        node = null;
      }
      if (node === null) {
        return LudusHost.openAt(dirfd, path, flags, varargs);
      }
      if (!LudusHost.canSleep()) {
        LudusHost.report('ludushostreadfailed', node.ludusHost.state, LudusHost.relativePath(node) ?? resolved,
                         'a nested call cannot wait for disk');
        return -{{{ cDefs.EIO }}};
      }
    }
    return Asyncify.handleSleep(wakeUp => {
      LudusHost.suspend(LudusHost.materialize(node, LudusHost.relativePath(node) ?? resolved),
                        errno => wakeUp(errno ? -errno : LudusHost.openAt(dirfd, path, flags, varargs)));
    });
  },

  // rename() is how QSaveFile and Ludus persistence publish a save: wait for the
  // folder, so a failed disk write fails the save that caused it.
  __syscall_renameat__deps: ['$LudusHost', '$SYSCALLS', '$Asyncify'],
  __syscall_renameat__async: true,
  __syscall_renameat: function(olddirfd, oldpath, newdirfd, newpath) {
    let state = null;
    if (Asyncify.state === Asyncify.State.Normal) {
      const result = LudusHost.renameAt(olddirfd, oldpath, newdirfd, newpath);
      try {
        const node = FS.lookupPath(SYSCALLS.calculateAt(newdirfd, SYSCALLS.getStr(newpath))).node;
        state = node.ludusHost?.state ?? null;
      } catch {
        state = null;
      }
      if (state === null || !LudusHost.hasPending(state) || !LudusHost.canSleep()) {
        if (state !== null) {
          LudusHost.scheduleFlush(state);
        }
        return result;
      }
      if (result < 0) {
        LudusHost.scheduleFlush(state);
        return result;
      }
    }
    return Asyncify.handleSleep(wakeUp => {
      LudusHost.suspend(LudusHost.flush(state), errno => wakeUp(errno ? -errno : 0));
    });
  },

  LudusHostPickFolder__deps: ['$LudusHost', '$Emval', '$UTF8ToString'],
  LudusHostPickFolder: function(targetHandle, intent, optionsJson) {
    const target = Emval.toValue(targetHandle);
    const options = JSON.parse(UTF8ToString(optionsJson));
    if (LudusHost.supportsHandles()) {
      LudusHost.pickWithHandle(target, intent, options).catch(error => LudusHost.dispatch(
        target, 'ludushostfailed', {failure: 'unreadable', path: '.', error: LudusHost.describeError(error)}));
    } else {
      LudusHost.pickWithInput(target, intent, options);
    }
  },

  LudusHostReopenFolder__deps: ['$LudusHost', '$Emval', '$UTF8ToString'],
  LudusHostReopenFolder: function(targetHandle, optionsJson) {
    const target = Emval.toValue(targetHandle);
    const options = JSON.parse(UTF8ToString(optionsJson));
    if (!LudusHost.supportsHandles()) {
      LudusHost.dispatch(target, 'ludushostfailed', {failure: 'no-remembered'});
      return;
    }
    LudusHost.reopen(target, options).catch(error => LudusHost.dispatch(
      target, 'ludushostfailed', {failure: 'unreadable', path: '.', error: LudusHost.describeError(error)}));
  },

  LudusHostMountSelection__deps: ['$LudusHost', '$UTF8ToString'],
  LudusHostMountSelection: function(selection, mountPoint, budgetBytes, optionsJson) {
    const options = JSON.parse(UTF8ToString(optionsJson));
    const pending = LudusHost.selections.get(selection);
    if (pending) {
      pending.skip = options.skip;
    }
    return LudusHost.mount(selection, UTF8ToString(mountPoint), budgetBytes);
  },

  LudusHostDiscardSelection__deps: ['$LudusHost'],
  LudusHostDiscardSelection: function(selection) {
    LudusHost.selections.delete(selection);
  },

  LudusHostSetMonitor__deps: ['$LudusHost', '$Emval'],
  LudusHostSetMonitor: function(targetHandle) {
    LudusHost.monitor = targetHandle ? Emval.toValue(targetHandle) : null;
  },

  LudusHostIsWritable__deps: ['$LudusHost', '$UTF8ToString'],
  LudusHostIsWritable: function(path) {
    const text = UTF8ToString(path);
    for (const state of LudusHost.mounts.values()) {
      if (text === state.rootPath || text.startsWith(state.rootPath + '/')) {
        return state.writable ? 1 : 0;
      }
    }
    return 0;
  },
});
