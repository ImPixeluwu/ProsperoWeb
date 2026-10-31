// SPDX-License-Identifier: GPL-3.0-or-later
// Unit test of src/chrome-ui/chrome.js under node with a minimal DOM shim:
// renders must patch the DOM, never recreate elements whose state did not
// change (a recreated spinner restarts its CSS animation).
//
//   node tests/chrome-ui/test_chrome.js src/chrome-ui/chrome.js
'use strict';

const fs = require('fs');
const vm = require('vm');

// ---- DOM shim --------------------------------------------------------------

let created = 0;

class ClassList {
  constructor(node) { this.node = node; }
  list() { return this.node.className.split(/\s+/).filter(Boolean); }
  contains(c) { return this.list().includes(c); }
  toggle(c, force) {
    const has = this.contains(c);
    const want = force === undefined ? !has : !!force;
    if (want !== has) {
      this.node.className = want ? this.list().concat(c).join(' ')
                                 : this.list().filter((x) => x !== c).join(' ');
    }
    return want;
  }
}

class Node {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.parentNode = null;
    this.className = '';
    this.dataset = {};
    this.hidden = false;
    this.title = '';
    this.value = '';
    this.id = '';
    this.listeners = {};
    this.writes = 0;  // textContent assignments
    this._text = '';
    this.scrollLeft = 0;
    this.clientWidth = 1000;
    this.offsetLeft = 0;
    const props = {};
    this.style = {
      setProperty(k, v) { props[k] = v; },
      getPropertyValue(k) { return props[k]; },
    };
    this.classList = new ClassList(this);
  }
  get textContent() {
    return this.children.length ? this.children.map((c) => c.textContent).join('') : this._text;
  }
  set textContent(v) {
    this.writes++;
    for (const c of this.children) c.parentNode = null;
    this.children = [];
    this._text = String(v);
  }
  set innerHTML(v) { this.textContent = v; }
  appendChild(n) { return this.insertBefore(n, null); }
  insertBefore(n, ref) {
    if (n.parentNode) n.parentNode.removeChild(n);
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i < 0) this.children.push(n); else this.children.splice(i, 0, n);
    n.parentNode = this;
    return n;
  }
  removeChild(n) {
    this.children.splice(this.children.indexOf(n), 1);
    n.parentNode = null;
    return n;
  }
  addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
  click() { (this.listeners.click || []).forEach((fn) => fn({ stopPropagation() {} })); }
  scrollIntoView() { this.scrolled = (this.scrolled || 0) + 1; }
  closest() { return null; }
  select() {}
  blur() {}
  querySelectorAll(sel) {
    const cls = sel.replace(/^\./, '');
    const out = [];
    const walk = (n) => n.children.forEach((c) => {
      if (c.classList.contains(cls)) out.push(c);
      walk(c);
    });
    walk(this);
    return out;
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
}

const byId = {};
const document = {
  documentElement: new Node('html'),
  body: new Node('body'),
  activeElement: null,
  createElement(tag) { created++; return new Node(tag); },
  getElementById(id) {
    if (!byId[id]) {
      byId[id] = new Node(id === 'address' ? 'input' : 'div');
      byId[id].id = id;
    }
    return byId[id];
  },
  elementFromPoint() { return null; },
};
const events = [];
const window = {
  document,
  location: { search: '' },
  ps5chromeHost: { send: (e) => events.push(e) },
};
window.window = window;
const context = vm.createContext({
  window, document, console,
  getComputedStyle: () => ({ fontSize: '16px' }),
  JSON, Math, Object, Array, String, parseFloat,
});
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), context, { filename: 'chrome.js' });
const chrome = window.ps5chrome;

// ---- Test helpers ------------------------------------------------------------

let checks = 0;
let failures = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.error('FAIL: ' + what);
  }
}

function state(over) {
  const s = {
    tabs: [
      { id: 1, title: 'One', url: 'https://one.example/', active: true, loading: false },
      { id: 2, title: 'Two', url: 'https://two.example/', active: false, loading: false },
      { id: 3, title: '', url: 'https://three.example/', active: false, loading: true },
    ],
    address: { text: 'https://one.example/', editing: false },
    nav: { back: true, fwd: false, loading: true },
    chrome_visible: true,
    content: { x: 0, y: 96, width: 1920, height: 984 },
    pointer: { x: 100, y: 200, visible: true },
    osk: { open: false },
    notice: { kind: null },
    menu: { open: false, items: [] },
    focus: 'page',
  };
  return Object.assign(s, over || {});
}
const $ = (id) => document.getElementById(id);
const tabs = () => $('tabs').children;
const spinners = () => $('tabs').querySelectorAll('.tab-spinner');

// ---- Tests -------------------------------------------------------------------

check(events.length === 1 && events[0].type === 'ready', 'chrome reports ready on load');

chrome.render(state());
check(tabs().length === 3, 'three tab nodes');
check(spinners().length === 1 && spinners()[0].parentNode === tabs()[2],
      'one spinner, on the loading tab');
check(tabs()[2].textContent.includes('three.example'), 'untitled tab shows its host');
check($('spinner').hidden === false, 'toolbar spinner shown while loading');

const nodesBefore = Array.from(tabs());
const spinnerBefore = spinners()[0];
const createdBefore = created;
const reloadWrites = $('nav-reload').writes;
// The pointer moved: everything else is unchanged.
const moved = state();
moved.pointer = { x: 300, y: 400, visible: true };
chrome.render(moved);
check(created === createdBefore, 'a render with an unchanged state creates no element');
check(tabs().every((n, i) => n === nodesBefore[i]), 'tab nodes are kept');
check(spinners()[0] === spinnerBefore, 'the spinner node survives (its animation keeps running)');
check($('nav-reload').writes === reloadWrites, 'unchanged reload label is not rewritten');
check($('pointer').style.transform === 'translate(300px,400px)', 'render moves the pointer');

// pointer() moves the ring without touching anything else.
chrome.pointer(10.4, 20.6, true);
check($('pointer').style.transform === 'translate(10px,21px)', 'pointer() sets the transform');
check(created === createdBefore, 'pointer() creates no element');
chrome.pointer(10, 21, false);
check($('pointer').hidden === true, 'pointer() hides the ring');
const native = state();
native.pointer = { x: 5, y: 5, visible: true, native: true };
chrome.render(native);
check($('pointer').hidden === true, 'a natively drawn pointer stays hidden in the chrome');

// A title change only rewrites that title.
const titled = state();
titled.tabs[0].title = 'One (renamed)';
chrome.render(titled);
check(tabs()[0] === nodesBefore[0] && tabs()[0].textContent.startsWith('One (renamed)'),
      'renamed tab keeps its node and shows the new title');
check(created === createdBefore, 'renaming creates no element');

// Loading moves from tab 3 to tab 2.
const loading = state();
loading.tabs[2].loading = false;
loading.tabs[1].loading = true;
chrome.render(loading);
check(spinners().length === 1 && spinners()[0].parentNode === tabs()[1],
      'spinner moved to the loading tab');
check(tabs()[2] === nodesBefore[2], 'finished tab keeps its node');
const spinner2 = spinners()[0];
chrome.render(loading);
check(spinners()[0] === spinner2, 'a repeated render keeps the new spinner');

// Closing and reordering.
const closed = state();
closed.tabs = [closed.tabs[2], closed.tabs[0]];
chrome.render(closed);
check(tabs().length === 2 && tabs()[0] === nodesBefore[2] && tabs()[1] === nodesBefore[0],
      'closed tab removed, the others reordered without recreation');
tabs()[0].click();
check(events[events.length - 1].type === 'tab_select' && events[events.length - 1].id === 3,
      'a reused tab node still reports its own id');

// Toolbar.
const idle = state();
idle.nav.loading = false;
chrome.render(idle);
check($('spinner').hidden === true && $('nav-reload').textContent === '↻',
      'idle: spinner hidden, reload label');

// Menu: moving the selection keeps the items.
const menu = state();
menu.menu = { open: true, section: 'main', items: [
  { label: 'New tab', url: '', selected: true },
  { label: 'History', url: '', selected: false },
] };
chrome.render(menu);
const items = Array.from($('menu-items').children);
check(items.length === 2 && items[0].classList.contains('selected'), 'menu items rendered');
menu.menu.items[0].selected = false;
menu.menu.items[1].selected = true;
chrome.render(menu);
check($('menu-items').children[1] === items[1] && items[1].classList.contains('selected') &&
      !items[0].classList.contains('selected'), 'menu selection moves without recreation');
check(items[1].scrolled === 1, 'newly selected item scrolled into view');

// OSK: moving the key focus keeps the keys and the caret.
const osk = state();
osk.osk = { open: true, target: 'address', text: 'abc', caret_index: 3, shift: false,
            cursor: { row: 0, col: 0 },
            rows: [[{ label: 'a', code: 'a', width: 1 }, { label: 'b', code: 'b', width: 1 }]] };
chrome.render(osk);
const key0 = $('osk-keys').children[0].children[0];
const caret = $('osk-text').querySelector('.osk-caret');
check(key0.classList.contains('focused'), 'focused key marked');
osk.osk.cursor = { row: 0, col: 1 };
chrome.render(osk);
check($('osk-keys').children[0].children[0] === key0 && !key0.classList.contains('focused') &&
      $('osk-keys').children[0].children[1].classList.contains('focused'),
      'key focus moves without recreating the keys');
check($('osk-text').querySelector('.osk-caret') === caret, 'unchanged text keeps the caret node');

console.log(checks + ' checks, ' + failures + ' failures');
process.exit(failures ? 1 : 0);
