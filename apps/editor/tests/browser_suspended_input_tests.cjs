// Keyboard input while host_folder.js suspends the editor, without a browser.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../browser/suspended_input.js'), 'utf8');

function load() {
  let library;
  const listeners = [];
  const context = {
    addToLibrary: value => { library = value; },
    addEventListener: (type, listener, options) => listeners.push({type, listener, options}),
  };
  context.globalThis = context;
  vm.runInNewContext(source, context);
  const input = library.$LudusSuspendedInput;
  // The library refers to itself by its Emscripten name.
  context.LudusSuspendedInput = input;
  return {input, listeners};
}

function key(type, value, extra = {}) {
  return {
    type, key: value, isTrusted: true, isComposing: false, keyCode: 0, ctrlKey: false, metaKey: false, ...extra,
    isPrevented: false, isStopped: false,
    preventDefault() { this.isPrevented = true; },
    stopPropagation() { this.isStopped = true; },
    stopImmediatePropagation() { this.isStopped = true; },
  };
}

function send({listeners}, event) {
  for (const {type, listener, options} of listeners) {
    if (type === event.type) {
      assert.equal(options.capture, true, 'the default must be cancelled before the focused input element');
      listener(event);
    }
  }
  return event;
}

test('keys pressed while a file call is suspended lose only their browser default', () => {
  const loaded = load();
  loaded.input.begin();
  const typed = ['Home', 'End', '1', '.'].flatMap(value => ['keydown', 'keyup'].map(type => send(loaded, key(type, value))));
  for (const event of typed) {
    assert.ok(event.isPrevented, 'the hidden input element must not take the key before Qt');
    assert.equal(event.isStopped, false, "the key must still reach Qt's queue among other input");
  }
  loaded.input.end();
  assert.equal(send(loaded, key('keydown', 'a')).isPrevented, false, 'Qt cancels keys itself while it runs');
});

test('the default stays cancelled until the last nested suspension ends', () => {
  const loaded = load();
  loaded.input.begin();
  loaded.input.begin(); // The resumed call suspended again before the first one ended.
  loaded.input.end();
  assert.ok(send(loaded, key('keydown', 'b')).isPrevented);
  loaded.input.end();
  loaded.input.end(); // An unmatched end does not start cancelling again.
  assert.equal(send(loaded, key('keydown', 'c')).isPrevented, false);
});

test('dispatched, composition and clipboard keys keep their browser default', () => {
  const loaded = load();
  loaded.input.begin();
  for (const event of [
    key('keydown', 'b', {isTrusted: false}),
    key('keydown', 'Process', {isComposing: true}),
    key('keydown', 'Unidentified', {keyCode: 229}),
    key('keydown', 'v', {ctrlKey: true}),
    key('keydown', 'C', {metaKey: true}),
    key('keydown', 'x', {ctrlKey: true}),
  ]) {
    assert.equal(send(loaded, event).isPrevented, false, event.key);
  }
  loaded.input.end();
});
