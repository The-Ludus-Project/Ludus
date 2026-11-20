// Keyboard input while host_folder.js suspends the editor for a file call. See
// docs/architecture/editor-browser-strategy.md (host project folders).
//
// Qt queues browser events that arrive while the Wasm instance is suspended and
// handles them after it resumes. A queued keydown therefore reaches Qt after the
// browser has run its default action: with a text field focused, Qt's hidden
// input element has already taken the key (caret moves, inserted text), and
// Qt's own preventDefault() comes too late. Typing right after a save then lands
// out of order. While a host-folder call is suspended, this library cancels
// trusted keydown/keyup events before Qt or the input element sees them and,
// once the call has resumed, dispatches equivalent events in their original
// order. Qt types text from keydown itself, and a dispatched (untrusted) event
// has no default action of its own.
//
// Thanks to the WHATWG DOM editors, "DOM Standard", sections "Dispatching
// events" (capturing listeners, isTrusted) and "Event.composedPath()"; and to
// the W3C UI Events editors, "UI Events", section "KeyboardEvent" (init dictionary
// and default actions).
// https://dom.spec.whatwg.org/#dispatching-events and https://www.w3.org/TR/uievents/#interface-keyboardevent
//
// Left alone: IME composition keys (they belong to the composition the browser
// is running) and the copy, cut and paste shortcuts (Qt needs their native
// clipboard events).

addToLibrary({
  $LudusSuspendedInput: {
    // Host-folder calls currently suspended; nested calls each count.
    suspensions: 0,
    // Cancelled keyboard events awaiting dispatch: {target, type, init}.
    held: [],
    isListening: false,

    // Call when a host-folder call suspends the instance.
    begin() {
      LudusSuspendedInput.suspensions += 1;
      if (!LudusSuspendedInput.isListening && typeof globalThis.addEventListener === 'function') {
        LudusSuspendedInput.isListening = true;
        for (const type of ['keydown', 'keyup']) {
          globalThis.addEventListener(type, LudusSuspendedInput.hold, {capture: true});
        }
      }
    },

    // Call after waking that call: the instance has suspended again or returned.
    end() {
      LudusSuspendedInput.suspensions = Math.max(0, LudusSuspendedInput.suspensions - 1);
      if (LudusSuspendedInput.suspensions === 0) {
        LudusSuspendedInput.replay();
      }
    },

    isLeftToBrowser(event) {
      if (event.isComposing || event.keyCode === 229) {
        return true;
      }
      const key = typeof event.key === 'string' ? event.key.toLowerCase() : '';
      return (event.ctrlKey || event.metaKey) && (key === 'c' || key === 'x' || key === 'v');
    },

    hold(event) {
      if (LudusSuspendedInput.suspensions === 0 || !event.isTrusted || LudusSuspendedInput.isLeftToBrowser(event)) {
        return;
      }
      // composedPath() is only available during dispatch; it names the element inside Qt's shadow root.
      const target = event.composedPath()[0] ?? event.target;
      event.preventDefault();
      event.stopImmediatePropagation();
      LudusSuspendedInput.held.push({target, type: event.type, init: {
        key: event.key, code: event.code, location: event.location, repeat: event.repeat,
        ctrlKey: event.ctrlKey, shiftKey: event.shiftKey, altKey: event.altKey, metaKey: event.metaKey,
        bubbles: true, cancelable: true, composed: true,
      }});
    },

    replay() {
      const held = LudusSuspendedInput.held;
      LudusSuspendedInput.held = [];
      // Qt queues these in order and handles them once this JavaScript returns.
      for (const {target, type, init} of held) {
        if (target?.isConnected !== false) {
          target.dispatchEvent(new KeyboardEvent(type, init));
        }
      }
    },
  },
});
