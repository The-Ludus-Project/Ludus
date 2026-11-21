// Input while host_folder.js suspends the editor for a file call. See
// docs/architecture/editor-browser-strategy.md (host project folders).
//
// Qt queues browser events that arrive while the Wasm instance is suspended and
// handles them, in arrival order, after it resumes. Qt cancels the browser
// default of the input it handles, but only when it handles the queued event:
// by then the browser has already run that default. While a host-folder call is
// suspended, this library therefore cancels, in the capture phase, the default
// actions Qt would have cancelled. The events still reach Qt's queue in their
// place among other input.
//
// - Trusted keydown/keyup: with a text field focused, Qt's hidden input element
//   would take the key first (caret moves, inserted text). Qt types text from
//   keydown itself.
// - Trusted pointerdown/mousedown inside a Qt window: no element of a Qt window
//   is focusable except Qt's hidden focus and input elements, so the browser's
//   mousedown default would move focus from them to the document body. Keys
//   typed before Qt handled the queued click then never reached Qt's window,
//   and were lost. Cancelling pointerdown also suppresses its compatibility
//   mousedown; cancelling mousedown covers a pointerdown that was not sent.
//
// Thanks to the WHATWG DOM editors, "DOM Standard", sections "Dispatching
// events" (capturing listeners, isTrusted, composedPath()) and "Canceling
// events"; to the W3C UI Events editors, "UI Events", sections "KeyboardEvent"
// and "MouseEvent" (default actions); to the W3C Pointer Events editors,
// "Pointer Events", section "Compatibility mapping with mouse events"; and to
// the WHATWG HTML editors, "HTML Standard", section "Focus" (focusing a clicked
// element's focusable ancestor, else the viewport).
// https://dom.spec.whatwg.org/#dispatching-events, https://www.w3.org/TR/uievents/,
// https://www.w3.org/TR/pointerevents/#compatibility-mapping-with-mouse-events and
// https://html.spec.whatwg.org/multipage/interaction.html#focus
//
// Left alone: IME composition keys (they belong to the composition the browser
// is running), the copy, cut and paste shortcuts (Qt needs their native
// clipboard events), and pointer input outside Qt's windows (the page's own
// controls keep their behaviour).

addToLibrary({
  $LudusSuspendedInput: {
    // Host-folder calls currently suspended; nested calls each count.
    suspensions: 0,
    isListening: false,
    // Class of the element that holds each Qt window, its frame and contents (Qt 6.10 QWasmWindow).
    qtWindowClass: 'qt-decorated-window',

    // Call when a host-folder call suspends the instance.
    begin() {
      LudusSuspendedInput.suspensions += 1;
      if (!LudusSuspendedInput.isListening && typeof globalThis.addEventListener === 'function') {
        LudusSuspendedInput.isListening = true;
        for (const type of ['keydown', 'keyup']) {
          globalThis.addEventListener(type, LudusSuspendedInput.cancelKeyDefault, {capture: true});
        }
        for (const type of ['pointerdown', 'mousedown']) {
          globalThis.addEventListener(type, LudusSuspendedInput.cancelPointerDefault, {capture: true});
        }
      }
    },

    // Call after waking that call: the instance has suspended again or returned.
    end() {
      LudusSuspendedInput.suspensions = Math.max(0, LudusSuspendedInput.suspensions - 1);
    },

    isSuspended(event) {
      return LudusSuspendedInput.suspensions !== 0 && event.isTrusted === true;
    },

    isLeftToBrowser(event) {
      if (event.isComposing || event.keyCode === 229) {
        return true;
      }
      const key = typeof event.key === 'string' ? event.key.toLowerCase() : '';
      return (event.ctrlKey || event.metaKey) && (key === 'c' || key === 'x' || key === 'v');
    },

    // Qt's windows live in an open shadow root, which composedPath() includes during dispatch.
    isInQtWindow(event) {
      const path = typeof event.composedPath === 'function' ? event.composedPath() : [];
      return path.some(node => node?.classList?.contains?.(LudusSuspendedInput.qtWindowClass) === true);
    },

    cancelKeyDefault(event) {
      if (LudusSuspendedInput.isSuspended(event) && !LudusSuspendedInput.isLeftToBrowser(event)) {
        event.preventDefault();
      }
    },

    cancelPointerDefault(event) {
      if (LudusSuspendedInput.isSuspended(event) && LudusSuspendedInput.isInQtWindow(event)) {
        event.preventDefault();
      }
    },
  },
});
