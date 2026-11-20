// Keyboard input held while host_folder.js suspends the editor, without a browser.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../browser/suspended_input.js'), 'utf8');

class KeyboardEvent {
  constructor(type, init) {
    Object.assign(this, init, {type, isTrusted: false});
  }
}

function load() {
  let library;
  const listeners = [];
  const context = {
    addToLibrary: value => { library = value; },
    addEventListener: (type, listener, options) => listeners.push({type, listener, options}),
    KeyboardEvent,
  };
  context.globalThis = context;
  vm.runInNewContext(source, context);
  const input = library.$LudusSuspendedInput;
  // The library refers to itself by its Emscripten name.
  context.LudusSuspendedInput = input;
  return {input, listeners};
}

function target() {
  const received = [];
  return {received, isConnected: true, dispatchEvent: event => { received.push(event); return true; }};
}

function key(element, type, value, extra = {}) {
  return {
    type, key: value, code: 'Key' + value, location: 0, repeat: false, isTrusted: true, isComposing: false,
    keyCode: 0, ctrlKey: false, shiftKey: false, altKey: false, metaKey: false, target: {}, ...extra,
    isPrevented: false, isStopped: false,
    composedPath: () => [element],
    preventDefault() { this.isPrevented = true; },
    stopImmediatePropagation() { this.isStopped = true; },
  };
}

function send({listeners}, event) {
  for (const {type, listener, options} of listeners) {
    if (type === event.type) {
      assert.equal(options.capture, true, 'the hold must run before Qt and the focused input element');
      listener(event);
    }
  }
  return event;
}

test('keys typed while a file call is suspended reach Qt in order after it resumes', () => {
  const loaded = load();
  const field = target();
  loaded.input.begin();
  const typed = ['Home', 'End', '1', '.'].flatMap(value =>
    ['keydown', 'keyup'].map(type => send(loaded, key(field, type, value, {shiftKey: value === 'End'}))));
  for (const event of typed) {
    assert.ok(event.isPrevented, 'the browser must not apply the key to the hidden input first');
    assert.ok(event.isStopped, 'Qt must not queue the original event');
  }
  assert.equal(field.received.length, 0);
  loaded.input.end();
  assert.deepEqual(field.received.map(event => [event.type, event.key, event.shiftKey]),
    typed.map(event => [event.type, event.key, event.shiftKey]));
  for (const event of field.received) {
    assert.ok(event instanceof KeyboardEvent);
    assert.equal(event.bubbles, true);
    assert.equal(event.composed, true, 'Qt listens outside the shadow root that holds its input element');
  }
  assert.equal(loaded.input.held.length, 0);
});

test('input waits for the last of nested suspensions', () => {
  const loaded = load();
  const field = target();
  loaded.input.begin();
  send(loaded, key(field, 'keydown', 'a'));
  loaded.input.begin(); // The resumed call suspended again before the first one ended.
  loaded.input.end();
  assert.equal(field.received.length, 0);
  send(loaded, key(field, 'keydown', 'b'));
  loaded.input.end();
  assert.deepEqual(field.received.map(event => event.key), ['a', 'b']);
});

test('input outside a suspension, dispatched input and browser-owned keys pass through', () => {
  const loaded = load();
  const field = target();
  loaded.input.begin();
  loaded.input.end();
  const idle = send(loaded, key(field, 'keydown', 'a'));
  loaded.input.begin();
  const passing = [
    key(field, 'keydown', 'b', {isTrusted: false}),
    key(field, 'keydown', 'Process', {isComposing: true}),
    key(field, 'keydown', 'Unidentified', {keyCode: 229}),
    key(field, 'keydown', 'v', {ctrlKey: true}),
    key(field, 'keydown', 'C', {metaKey: true}),
  ].map(event => send(loaded, event));
  loaded.input.end();
  for (const event of [idle, ...passing]) {
    assert.equal(event.isPrevented, false, event.key);
    assert.equal(event.isStopped, false, event.key);
  }
  assert.equal(field.received.length, 0);
});

test('a target removed while held is skipped', () => {
  const loaded = load();
  const field = target();
  loaded.input.begin();
  send(loaded, key(field, 'keydown', 'a'));
  field.isConnected = false;
  loaded.input.end();
  assert.equal(field.received.length, 0);
});
