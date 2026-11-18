// Thanks to Qt Group, "Qt for WebAssembly", Using qtloader: load the existing
// Widgets application into a page-owned container, with explicit exit/failure UI.
// https://doc.qt.io/qt-6/wasm.html#using-qtloader
"use strict";

const loading = document.getElementById("loading");
const status = document.getElementById("status");
const retry = document.getElementById("retry");
const screen = document.getElementById("screen");
globalThis.ludusPlaySample = function(id) {
  const titles = {'cornell-box': 'Cornell Box', 'live-edit-game': 'Live Edit Game', 'scripted-game': 'Scripted Game'};
  if (!Object.hasOwn(titles, id)) return;
  const dialog = document.getElementById('web-player');
  const frame = document.getElementById('web-player-frame');
  document.getElementById('web-player-title').textContent = titles[id];
  frame.src = 'players/' + id + '/index.html';
  dialog.showModal();
  frame.focus();
};
document.getElementById('web-player-close').addEventListener('click', () => document.getElementById('web-player').close());
document.getElementById('web-player').addEventListener('close', () => {
  document.getElementById('web-player-frame').src = 'about:blank';
});
window.addEventListener('message', event => {
  if (event.origin === location.origin && event.source === document.getElementById('web-player-frame').contentWindow &&
      event.data?.type === 'ludus-close-player') document.getElementById('web-player').close();
});
retry.addEventListener("click", () => location.reload());
window.addEventListener("beforeunload", event => {
  // Saves to a folder opened from disk may still be reaching it (host_folder.js).
  if (globalThis.ludusEditorEdited || (globalThis.ludusHostPendingWrites?.() ?? 0) > 0) {
    event.preventDefault();
    event.returnValue = "";
  }
});

function failed(message) {
  loading.hidden = false;
  status.textContent = message;
  retry.hidden = false;
  document.body.dataset.editorState = "failed";
}

async function start() {
  document.body.dataset.editorState = "loading";
  try {
    if (typeof qtLoad !== "function" || typeof ludus_editor_entry !== "function") {
      throw new Error("Editor assets are unavailable.");
    }
    if (typeof WebAssembly !== "object") {
      throw new Error("WebAssembly is unavailable.");
    }
    await qtLoad({
      qt: {
        entryFunction: ludus_editor_entry,
        containerElements: [screen],
        onLoaded: () => {
          loading.hidden = true;
          document.body.dataset.editorState = "ready";
        },
        onExit: () => failed("The editor closed. Reload to start another session. Download your changes before leaving."),
      },
    });
  } catch (error) {
    console.error("Ludus editor startup failed", error);
    failed("The editor could not start. Retry in a browser with WebAssembly and WebGL enabled, or use the desktop editor.");
  }
}
start();
