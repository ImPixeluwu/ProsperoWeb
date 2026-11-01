// SPDX-License-Identifier: GPL-3.0-or-later
// Browser chrome renderer. The shell (ShellController) owns all state and
// calls window.ps5chrome.render(state); user interaction is reported back
// as events, never applied locally. No frameworks, no network.
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var lastState = null;

  // ---- Events to the host ------------------------------------------------

  // Events are posted as plain objects; the frontends hand the shell a
  // parsed JSON object (a JSON string is accepted too, see on_chrome_event).
  function send(event) {
    try {
      var handlers = window.webkit && window.webkit.messageHandlers;
      if (handlers && handlers.chrome) {
        handlers.chrome.postMessage(event);
        return;
      }
      if (window.ps5chromeHost && typeof window.ps5chromeHost.send === 'function') {
        window.ps5chromeHost.send(event);
        return;
      }
    } catch (e) {
      // fall through to logging
    }
    console.log('ps5chrome event', JSON.stringify(event));
  }

  // ---- Helpers -----------------------------------------------------------

  function el(tag, cls, text) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function hostOf(url) {
    var m = /^[a-z][a-z0-9+.-]*:\/\/([^/?#]*)/i.exec(url || '');
    if (!m) return url || '';
    return m[1].replace(/^.*@/, '');
  }

  function tabLabel(tab) {
    if (tab.title) return tab.title;
    if (!tab.url || tab.url === 'about:blank') return 'New tab';
    return hostOf(tab.url);
  }

  // ---- Rendering ---------------------------------------------------------
  //
  // render() runs whenever the shell state changes, so it patches the DOM
  // instead of rebuilding it: an element whose state did not change is never
  // recreated or written to. Recreating the tab spinners restarted their CSS
  // animation on every render, which froze them while the pointer moved.

  function setText(node, text) {
    if (node.textContent !== text) node.textContent = text;
  }

  function setHidden(node, hidden) {
    hidden = !!hidden;
    if (node.hidden !== hidden) node.hidden = hidden;
  }

  function setClass(node, cls) {
    if (node.className !== cls) node.className = cls;
  }

  function setTitle(node, title) {
    if (node.title !== title) node.title = title;
  }

  // Tab nodes by tab id; each keeps its title, spinner and close button.
  var tabNodes = {};

  function makeTab(id) {
    var node = el('div', 'tab');
    node.dataset.id = id;
    var entry = { node: node, title: el('span', 'tab-title'), spinner: null };
    node.appendChild(entry.title);
    var close = el('button', 'tab-close', '✕');
    close.title = 'Close tab';
    close.addEventListener('click', function (ev) {
      ev.stopPropagation();
      send({ type: 'tab_close', id: id });
    });
    node.appendChild(close);
    node.addEventListener('click', function () { send({ type: 'tab_select', id: id }); });
    return entry;
  }

  function renderTabs(state) {
    var box = $('tabs');
    var seen = {};
    (state.tabs || []).forEach(function (tab, index) {
      var key = String(tab.id);
      seen[key] = true;
      var entry = tabNodes[key] || (tabNodes[key] = makeTab(tab.id));
      setClass(entry.node, 'tab' + (tab.active ? ' active' : ''));
      setText(entry.title, tabLabel(tab));
      if (tab.loading && !entry.spinner) {
        entry.spinner = el('span', 'tab-spinner');
        entry.node.insertBefore(entry.spinner, entry.title);
      } else if (!tab.loading && entry.spinner) {
        entry.node.removeChild(entry.spinner);
        entry.spinner = null;
      }
      var at = box.children[index] || null;
      if (at !== entry.node) box.insertBefore(entry.node, at);
    });
    Object.keys(tabNodes).forEach(function (key) {
      if (seen[key]) return;
      var node = tabNodes[key].node;
      if (node.parentNode) node.parentNode.removeChild(node);
      delete tabNodes[key];
    });
  }

  function renderToolbar(state) {
    var nav = state.nav || {};
    $('nav-back').classList.toggle('disabled', !nav.back);
    $('nav-fwd').classList.toggle('disabled', !nav.fwd);
    setText($('nav-reload'), nav.loading ? '✕' : '↻');
    setTitle($('nav-reload'), nav.loading ? 'Stop' : 'Reload (Create)');
    setHidden($('spinner'), !nav.loading);

    var address = state.address || {};
    var input = $('address');
    var typing = document.activeElement === input && state.focus === 'chrome';
    var text = address.text || '';
    if (!typing && input.value !== text) input.value = text;
    $('address-form').classList.toggle('editing', !!address.editing);
    var secure = /^https:/i.test(text) && !address.editing;
    var lock = $('address-lock');
    setText(lock, secure ? '\u{1F512}︎' : '');
    lock.classList.toggle('secure', secure);
  }

  var menuKey = null;
  var menuSelected = -1;

  function renderMenu(state) {
    var menu = state.menu || {};
    setHidden($('menu'), !menu.open);
    if (!menu.open) {
      menuKey = null;
      return;
    }
    var titles = { main: 'Menu', bookmarks: 'Bookmarks', history: 'History', addons: 'Add-ons' };
    setText($('menu-title'), titles[menu.section] || 'Menu');
    var items = menu.items || [];
    var list = $('menu-items');
    var key = JSON.stringify([menu.section, items.map(function (item) {
      return [item.label, item.url || ''];
    })]);
    if (key !== menuKey) {
      menuKey = key;
      menuSelected = -1;
      list.textContent = '';
      items.forEach(function (item, index) {
        var li = el('li', 'menu-item');
        li.appendChild(el('span', 'label', item.label));
        if (item.url) li.appendChild(el('span', 'url', item.url));
        li.addEventListener('click', function () { send({ type: 'menu_select', index: index }); });
        list.appendChild(li);
      });
    }
    var selected = -1;
    items.forEach(function (item, index) {
      if (item.selected) selected = index;
      setClass(list.children[index], 'menu-item' + (item.selected ? ' selected' : ''));
    });
    if (selected !== menuSelected) {
      menuSelected = selected;
      var node = list.children[selected];
      if (node && node.scrollIntoView) node.scrollIntoView({ block: 'nearest' });
    }
  }

  function renderNotice(state) {
    var notice = state.notice || {};
    var open = !!notice.kind;
    setHidden($('notice-backdrop'), !open);
    if (!open) return;
    setClass($('notice'), 'notice ' + notice.kind);
    setText($('notice-icon'), notice.kind === 'info' ? 'i' : '!');
    setText($('notice-title'), notice.title || '');
    setText($('notice-text'), notice.text || '');
    setHidden($('notice-cancel'), notice.kind !== 'drm');
  }

  var OSK_SPECIAL = ['shift', 'layout', 'clear', 'left', 'space', 'right', 'backspace', 'enter'];

  // Draws text with a blinking caret after caretIndex code points (the
  // shell counts code points, so split with Array.from, not UTF-16 units)
  // and scrolls the line so the caret stays visible in long URLs.
  function renderOskText(box, text, caretIndex) {
    var chars = Array.from(text);
    var at = typeof caretIndex === 'number' ? caretIndex : chars.length;
    at = Math.max(0, Math.min(chars.length, at));
    box.textContent = '';
    box.appendChild(el('span', 'osk-before', chars.slice(0, at).join('')));
    var caret = box.appendChild(el('span', 'osk-caret'));
    box.appendChild(el('span', 'osk-after', chars.slice(at).join('')));

    var margin = 3 * parseFloat(getComputedStyle(document.documentElement).fontSize);
    var x = caret.offsetLeft;
    var view = box.clientWidth;
    if (x - box.scrollLeft > view - margin) {
      box.scrollLeft = x - view + margin;
    } else if (x - box.scrollLeft < margin) {
      box.scrollLeft = Math.max(0, x - margin);
    }
  }

  function oskKeyClass(osk, key) {
    var cls = 'osk-key' + (OSK_SPECIAL.indexOf(key.code) >= 0 ? ' special' : '');
    if (key.code === 'shift' && osk.shift) cls += ' shift-on';
    if (key.code === 'clear') cls += ' clear';
    return cls;
  }

  var oskTextKey = null;
  var oskKeysKey = null;

  function renderOsk(state) {
    var osk = state.osk || {};
    setHidden($('osk'), !osk.open);
    if (!osk.open) {
      oskTextKey = null;
      return;
    }
    setText($('osk-target'), osk.target === 'address' ? 'Address' : 'Text field');
    // Redrawn only when it changed: redrawing restarts the caret blink.
    var textKey = JSON.stringify([osk.text || '', osk.caret_index]);
    if (textKey !== oskTextKey) {
      oskTextKey = textKey;
      renderOskText($('osk-text'), osk.text || '', osk.caret_index);
    }
    var rows = osk.rows || [];
    var keys = $('osk-keys');
    var keysKey = JSON.stringify(rows);
    if (keysKey !== oskKeysKey) {
      oskKeysKey = keysKey;
      keys.textContent = '';
      rows.forEach(function (row, r) {
        var rowEl = el('div', 'osk-row');
        row.forEach(function (key, c) {
          var label = key.label;
          if (key.code === 'backspace') label = '⌫';
          if (key.code === 'enter') label = 'Go';
          if (key.code === 'shift') label = '⇧';
          var b = el('button', 'osk-key', label);
          b.style.flexGrow = String(key.width || 1);
          b.style.maxWidth = (8 * (key.width || 1)) + 'rem';
          b.addEventListener('click', function () { send({ type: 'osk_key', row: r, col: c }); });
          rowEl.appendChild(b);
        });
        keys.appendChild(rowEl);
      });
    }
    // Focus and Shift only change classes on the existing keys.
    var cursor = osk.cursor || { row: 0, col: 0 };
    rows.forEach(function (row, r) {
      var rowEl = keys.children[r];
      row.forEach(function (key, c) {
        var focused = r === cursor.row && c === cursor.col;
        setClass(rowEl.children[c], oskKeyClass(osk, key) + (focused ? ' focused' : ''));
      });
    });
  }

  // The pointer ring. Moving it only sets a transform: the shell calls
  // pointer() on every pointer move instead of a full render(). With
  // native set the frontend draws the ring itself and this one stays hidden.
  var pointerShown = { x: null, y: null, visible: false };

  function pointer(x, y, visible) {
    var node = $('pointer');
    visible = !!visible;
    setHidden(node, !visible);
    if (!visible) return;
    x = Math.round(x);
    y = Math.round(y);
    if (x === pointerShown.x && y === pointerShown.y) return;
    pointerShown.x = x;
    pointerShown.y = y;
    node.style.transform = 'translate(' + x + 'px,' + y + 'px)';
  }

  function renderPointer(state) {
    var p = state.pointer || {};
    pointer(p.x || 0, p.y || 0, p.visible && !p.native);
  }

  var bandShown = null;

  function render(state) {
    if (typeof state === 'string') state = JSON.parse(state);
    lastState = state;
    var content = state.content || {};
    var band = state.chrome_visible ? (content.y || 96) : 0;
    if (band !== bandShown) {
      bandShown = band;
      document.documentElement.style.setProperty('--band', band + 'px');
    }
    setHidden($('band'), !state.chrome_visible);
    renderTabs(state);
    renderToolbar(state);
    renderMenu(state);
    renderNotice(state);
    renderOsk(state);
    renderPointer(state);
    var focus = state.focus || 'page';
    if (document.body.dataset.focus !== focus) document.body.dataset.focus = focus;
  }

  // Gamepad Cross over the chrome: activate the element under the pointer.
  function describe(el) {
    if (!el) return 'nothing';
    var d = el.tagName.toLowerCase();
    if (el.id) d += '#' + el.id;
    var owner = el.closest('[id]');
    if (owner && owner !== el) d += ' in #' + owner.id;
    return d;
  }

  function click(x, y) {
    var target = document.elementFromPoint(x, y);
    var handler = !target ? 'none'
      : (target === $('address') || target.closest('#address-form')) ? 'address_focus' : 'click';
    // Frontends may log this; the shell ignores the type.
    send({ type: 'click_trace', x: x, y: y, target: describe(target), handler: handler });
    if (handler === 'address_focus') {
      send({ type: 'address_focus' });
    } else if (target) {
      target.click();
    }
  }

  // ---- Wiring ------------------------------------------------------------

  $('tab-new').addEventListener('click', function () { send({ type: 'tab_new' }); });
  $('nav-back').addEventListener('click', function () { send({ type: 'nav', action: 'back' }); });
  $('nav-fwd').addEventListener('click', function () { send({ type: 'nav', action: 'forward' }); });
  $('nav-reload').addEventListener('click', function () {
    var loading = lastState && lastState.nav && lastState.nav.loading;
    send({ type: 'nav', action: loading ? 'stop' : 'reload' });
  });
  $('menu-btn').addEventListener('click', function () {
    var open = lastState && lastState.menu && lastState.menu.open;
    send({ type: open ? 'menu_close' : 'menu_open' });
  });
  $('address-form').addEventListener('submit', function (ev) {
    ev.preventDefault();
    send({ type: 'navigate', text: $('address').value });
    $('address').blur();
  });
  $('address').addEventListener('focus', function () {
    send({ type: 'address_focus' });
    $('address').select();
  });
  $('address').addEventListener('blur', function () { send({ type: 'address_blur' }); });
  $('address').addEventListener('keydown', function (ev) {
    if (ev.key === 'Escape') $('address').blur();
  });
  $('notice-ok').addEventListener('click', function () {
    send({ type: 'notice_dismiss', proceed: true });
  });
  $('notice-cancel').addEventListener('click', function () {
    send({ type: 'notice_dismiss', proceed: false });
  });

  window.ps5chrome = { render: render, pointer: pointer, click: click, send: send };

  // ---- Demo states (chrome.html?demo=1|osk|notice|menu) --------------------

  function demoState(kind) {
    var latin = [
      '1234567890', 'qwertyuiop', 'asdfghjkl-', 'zxcvbnm,./'
    ].map(function (row) {
      return row.split('').map(function (ch) { return { label: ch, code: ch, width: 1 }; });
    });
    latin.push([
      { label: 'Shift', code: 'shift', width: 1.5 },
      { label: '?123', code: 'layout', width: 1.5 },
      { label: 'Clear', code: 'clear', width: 1.5 },
      { label: '←', code: 'left', width: 1 },
      { label: 'Space', code: 'space', width: 3 },
      { label: '→', code: 'right', width: 1 },
      { label: 'Backspace', code: 'backspace', width: 1.5 },
      { label: 'Enter', code: 'enter', width: 1.5 }
    ]);
    var state = {
      tabs: [
        { id: 1, title: 'PlayStation 5 - Wikipedia', url: 'https://en.wikipedia.org/wiki/PlayStation_5', active: true, loading: false },
        { id: 2, title: 'DuckDuckGo', url: 'https://duckduckgo.com/', active: false, loading: false },
        { id: 3, title: '', url: 'https://archive.org/details/BigBuckBunny_124', active: false, loading: true }
      ],
      address: { text: 'https://en.wikipedia.org/wiki/PlayStation_5', editing: false },
      nav: { back: true, fwd: false, loading: false },
      chrome_visible: true,
      fullscreen_state: 'normal',
      content: { x: 0, y: 96, width: 1920, height: 984 },
      pointer: { x: 1180, y: 520, visible: true },
      osk: { open: false, layout: 'latin', shift: false, text: '', caret_index: 0, target: 'address', cursor: { row: 0, col: 0 }, rows: [] },
      notice: { kind: null, title: '', text: '' },
      menu: { open: false, section: 'main', items: [], selected: 0 },
      focus: 'page'
    };
    if (kind === 'osk') {
      state.osk = { open: true, layout: 'latin', shift: false, text: 'ps5 homebrew', caret_index: 3, target: 'address', cursor: { row: 2, col: 3 }, rows: latin };
      state.address = { text: 'ps5 homebrew', editing: true };
      state.pointer.visible = false;
      state.focus = 'osk';
    } else if (kind === 'notice') {
      state.notice = {
        kind: 'drm',
        title: 'Protected video is not supported',
        text: 'This site requested protected (DRM) playback. This browser does not include a DRM module (Widevine, PlayReady or FairPlay), so protected videos cannot be played here.'
      };
      state.pointer.visible = false;
      state.focus = 'notice';
    } else if (kind === 'menu') {
      state.menu = {
        open: true, section: 'bookmarks', selected: 1,
        items: [
          { label: 'PlayStation 5 - Wikipedia', url: 'https://en.wikipedia.org/wiki/PlayStation_5', selected: false },
          { label: 'Big Buck Bunny', url: 'https://archive.org/details/BigBuckBunny_124', selected: true },
          { label: 'ps5-payload-dev', url: 'https://github.com/ps5-payload-dev', selected: false }
        ]
      };
      state.pointer.visible = false;
      state.focus = 'menu';
    }
    return state;
  }

  var demo = /[?&]demo=([^&]*)/.exec(window.location.search);
  if (demo) {
    document.body.classList.add('demo');
    render(demoState(decodeURIComponent(demo[1])));
  } else {
    send({ type: 'ready' });
  }
})();
