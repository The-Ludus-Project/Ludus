// Keyboard input while host_folder.js suspends the editor for a file call. See
// docs/architecture/editor-browser-strategy.md (host project folders).
//
// Qt queues browser events that arrive while the Wasm instance is suspended and
// handles them, in arrival order, after it resumes. A queued keydown therefore
// reaches Qt after the browser has run its default action: with a text field
// focused, Qt's hidden input element has already taken the key (caret moves,
// inserted text), and Qt's own preventDefault() comes too late. Typing right
// after a save then lands out of order. While a host-folder call is suspended,
// this library cancels the default action of trusted keydown/keyup events in
// the capture phase. The events still reach Qt's queue in their place among
// pointer and other input events, and Qt types text from keydown itself.
//
// Thanks to the WHATWG DOM editors, "DOM Standard", sections "Dispatching
// events" (capturing listeners, isTrusted) and "Canceling events"; and to the
// W3C UI Events editors, "UI Events", section "KeyboardEvent" (default actions).
// https://dom.spec.whatwg.org/#dispatching-events and https://www.w3.org/TR/uievents/#interface-keyboardevent
//
// Left alone: IME composition keys (they belong to the composition the browser
// is running) and the copy, cut and paste shortcuts (Qt needs their native
// clipboard events).

addToLibrary({
  $LudusSuspendedInput: {
    // Host-folder calls currently suspended; nested calls each count.
    suspensions: 0,
    isListening: false,

    // Call when a host-folder call suspends the instance.
    begin() {
      LudusSuspendedInput.suspensions += 1;
      if (!LudusSuspendedInput.isListening && typeof globalThis.addEventListener === 'function') {
        LudusSuspendedInput.isListening = true;
        for (const type of ['keydown', 'keyup']) {
          globalThis.addEventListener(type, LudusSuspendedInput.cancelDefault, {capture: true});
        }
      }
    },

    // Call after waking that call: the instance has suspended again or returned.
    end() {
      LudusSuspendedInput.suspensions = Math.max(0, LudusSuspendedInput.suspensions - 1);
    },

    isLeftToBrowser(event) {
      if (event.isComposing || event.keyCode === 229) {
        return true;
      }
      const key = typeof event.key === 'string' ? event.key.toLowerCase() : '';
      return (event.ctrlKey || event.metaKey) && (key === 'c' || key === 'x' || key === 'v');
    },

    cancelDefault(event) {
      if (LudusSuspendedInput.suspensions !== 0 && event.isTrusted && !LudusSuspendedInput.isLeftToBrowser(event)) {
        event.preventDefault();
      }
    },
  },
});
