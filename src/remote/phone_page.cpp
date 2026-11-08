// SPDX-License-Identifier: GPL-3.0-or-later
//
// The page phones, tablets and computers load from the remote control: PIN
// entry, then a trackpad (drag moves the pointer, tap clicks, two fingers or
// the mouse wheel scroll; on a computer the mouse can also be captured), an
// address bar, a text box for the focused field, the browser's buttons and
// keyboard shortcuts. The layout adapts from a phone held upright (one
// column) to sideways phones, tablets and desktops (pad beside the
// controls). Self-contained (no external resources), so it works without
// Internet access.
#include "prosperoweb/remote/remote_api.h"

namespace prosperoweb::remote {

const std::string& phone_page() {
  static const std::string page = R"PWHTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>ProsperoWeb remote</title>
<style>
:root { --bg:#101218; --panel:#1b1e27; --line:#2c3040; --hover:#242836; --text:#e8eaf0; --dim:#9aa0b2;
  --accent:#4c8dff; --warn:#e0a83a; --bad:#ff6b6b; --gap:10px; --btn-pad:10px; }
* { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
html, body { margin:0; height:100%; background:var(--bg); color:var(--text);
  font:16px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
body { display:flex; flex-direction:column; gap:var(--gap); max-width:1280px; margin:0 auto;
  padding:max(12px, env(safe-area-inset-top)) max(16px, env(safe-area-inset-right))
          max(12px, env(safe-area-inset-bottom)) max(16px, env(safe-area-inset-left)); }
header { display:flex; align-items:baseline; gap:12px; min-width:0; }
h1 { font-size:18px; margin:0; font-weight:600; white-space:nowrap; }
#status { color:var(--dim); font-size:13px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; margin:0; min-width:0; }
.row { display:flex; gap:8px; }
input, button { font:inherit; color:inherit; border-radius:10px; border:1px solid var(--line); background:var(--panel); }
input { flex:1; min-width:0; padding:var(--btn-pad) 12px; }
input:focus { outline:2px solid var(--accent); outline-offset:-1px; }
button { padding:var(--btn-pad) 12px; cursor:pointer; touch-action:manipulation; white-space:nowrap; }
button:active { background:var(--line); }
button:focus-visible { outline:2px solid var(--accent); outline-offset:1px; }
@media (hover:hover) { button:hover { background:var(--hover); } }
button.primary { background:var(--accent); border-color:var(--accent); color:#fff; }
.note { color:var(--dim); font-size:13px; margin:0; }
.warn { color:var(--warn); font-size:13px; margin:0; }
.err { color:var(--bad); font-size:14px; min-height:1.4em; margin:0; }
kbd { font:12px ui-monospace, monospace; border:1px solid var(--line); border-radius:4px; padding:0 4px; background:var(--panel); }

#pair { display:flex; flex-direction:column; gap:12px; width:100%; max-width:420px; margin:10vh auto 0; }
#pin { font-size:28px; letter-spacing:8px; text-align:center; }

#remote { display:none; flex:1; min-height:0; gap:var(--gap);
  grid-template-columns:1fr; grid-template-rows:auto 1fr auto auto auto;
  grid-template-areas:"url" "pad" "keys" "text" "err"; }
#remote.on { display:grid; }
#urlRow { grid-area:url; } #padWrap { grid-area:pad; } #keys { grid-area:keys; }
#textRow { grid-area:text; } #err { grid-area:err; } #help { grid-area:help; display:none; }
#side { display:contents; }
#padWrap { display:flex; flex-direction:column; gap:6px; min-height:0; }
#pad { flex:1; min-height:200px; border:1px solid var(--line); border-radius:14px; background:var(--panel);
  touch-action:none; display:flex; align-items:center; justify-content:center; text-align:center; padding:12px;
  color:var(--dim); user-select:none; -webkit-user-select:none; cursor:crosshair; }
#pad.locked { border-color:var(--accent); cursor:none; }
#padBar { display:none; justify-content:space-between; align-items:center; gap:8px; }
#keys { display:grid; grid-template-columns:repeat(4, 1fr); gap:8px; }
#keys button { padding-left:4px; padding-right:4px; }
.mouse-only { display:none; }

/* Mouse and trackpad computers: capture button, shortcuts. */
@media (pointer:fine) {
  .mouse-only { display:initial; }
  #padBar { display:flex; }
}

/* Desktops, landscape tablets and phones held sideways: pad on the left,
   controls on the right. Upright tablets keep one column (a wide pad). */
@media (min-width:1000px), (orientation:landscape) and (min-width:560px) {
  #remote { grid-template-columns:minmax(0, 1fr) minmax(260px, 360px);
    grid-template-rows:minmax(0, 1fr); grid-template-areas:"pad side"; }
  #side { display:flex; flex-direction:column; gap:var(--gap); grid-area:side; min-height:0; overflow-y:auto; }
  #help { display:block; }
  #pad { min-height:240px; }
}
@media (min-width:1100px) {
  #remote { grid-template-columns:minmax(0, 1fr) 380px; }
  #pad { font-size:18px; }
}
/* Short landscape phones: everything smaller so it fits without scrolling. */
@media (orientation:landscape) and (max-height:500px) {
  :root { --gap:6px; --btn-pad:6px; }
  body { padding-top:max(6px, env(safe-area-inset-top)); padding-bottom:max(6px, env(safe-area-inset-bottom)); }
  h1 { font-size:15px; }
  #pad { min-height:120px; }
  #help { display:none; }
  #pair { margin-top:2vh; }
}
</style>
</head>
<body>
<header><h1>ProsperoWeb remote</h1><p id="status"></p></header>

<section id="pair">
  <p class="note">Enter the PIN shown on the TV.</p>
  <input id="pin" inputmode="numeric" autocomplete="one-time-code" maxlength="6" placeholder="000000" aria-label="PIN">
  <button id="pairBtn" class="primary">Connect</button>
  <p id="pairErr" class="err" role="alert"></p>
  <p class="warn">The connection is not encrypted: anyone on this Wi-Fi network could read what you type. Avoid typing passwords here.</p>
</section>

<section id="remote">
  <div id="padWrap">
    <div id="pad" aria-label="Trackpad"><span id="padHint">Drag to move &middot; tap to click &middot; two fingers to scroll</span></div>
    <div id="padBar">
      <span class="note">Click the pad or press <kbd>Enter</kbd> to click &middot; wheel scrolls</span>
      <button id="lock" class="mouse-only" title="Use your mouse as the TV pointer (Esc releases it)">Capture mouse</button>
    </div>
  </div>
  <div id="side">
    <div id="urlRow" class="row">
      <input id="url" type="url" inputmode="url" autocapitalize="off" autocomplete="off" placeholder="Address or search" aria-label="Address">
      <button id="go" class="primary">Go</button>
      <button id="goTab" title="Open in a new tab">+Tab</button>
    </div>
    <div id="keys">
      <button data-a="back" title="Back (Backspace)">&larr;</button>
      <button data-a="forward" title="Forward (Alt+&rarr;)">&rarr;</button>
      <button data-a="reload" title="Reload">&#x21bb;</button>
      <button data-a="escape" title="Circle: close a menu or notice (Esc)">Esc</button>
      <button data-a="prev_tab" title="Previous tab ([)">Tab&lt;</button>
      <button data-a="next_tab" title="Next tab (])">Tab&gt;</button>
      <button data-a="new_tab" title="New tab">New</button>
      <button data-a="close_tab" title="Close tab">Close</button>
      <button data-a="page_up" title="Page up (PgUp, Shift+Space)">PgUp</button>
      <button data-a="page_down" title="Page down (PgDn, Space)">PgDn</button>
      <button data-a="fullscreen" title="Fullscreen (F)">Full</button>
      <button data-a="menu" title="Menu (M)">Menu</button>
    </div>
    <div id="textRow" class="row">
      <input id="text" autocomplete="off" placeholder="Type into the focused field" aria-label="Text to type">
      <button id="send">Send</button>
      <button id="enter" title="Send and press Enter">&#x23ce;</button>
    </div>
    <p id="err" class="err" role="alert"></p>
    <p id="help" class="note mouse-only">Keyboard: arrows scroll, <kbd>Space</kbd>/<kbd>PgDn</kbd> page down,
      <kbd>Backspace</kbd> back, <kbd>[</kbd> <kbd>]</kbd> tabs, <kbd>M</kbd> menu, <kbd>F</kbd> fullscreen,
      <kbd>Esc</kbd> close, <kbd>Enter</kbd> click, <kbd>/</kbd> address, <kbd>I</kbd> type text.</p>
  </div>
</section>

<script>
(function () {
  "use strict";
  var KEY = "pw-remote-token";
  var token = "";
  try { token = localStorage.getItem(KEY) || ""; } catch (e) {}
  var $ = function (id) { return document.getElementById(id); };
  var finePointer = window.matchMedia && matchMedia("(pointer:fine)").matches;
  if (finePointer && !matchMedia("(pointer:coarse)").matches) {
    $("padHint").textContent = "Drag with the mouse to move · click to click · wheel to scroll";
  }

  function api(method, path, body) {
    var headers = { "Content-Type": "application/json" };
    if (token) headers["X-PW-Token"] = token;
    return fetch(path, { method: method, headers: headers, body: body === undefined ? undefined : JSON.stringify(body) })
      .then(function (r) {
        return r.json().catch(function () { return {}; }).then(function (j) { j.httpStatus = r.status; return j; });
      });
  }

  function showPair(message) {
    token = "";
    try { localStorage.removeItem(KEY); } catch (e) {}
    $("remote").classList.remove("on");
    $("pair").style.display = "flex";
    $("status").textContent = "";
    $("pairErr").textContent = message || "";
    $("pin").focus();
  }
  function showRemote() {
    $("pair").style.display = "none";
    $("remote").classList.add("on");
    $("status").textContent = "Connected";
    poll();
  }

  $("pairBtn").onclick = function () {
    api("POST", "/api/pair", { pin: $("pin").value.trim() }).then(function (j) {
      if (j.token) {
        token = j.token;
        try { localStorage.setItem(KEY, token); } catch (e) {}
        $("pin").value = "";
        showRemote();
      } else if (j.httpStatus === 429) {
        $("pairErr").textContent = "Too many wrong PINs. Turn the remote control off and on again on the TV.";
      } else {
        $("pairErr").textContent = "Wrong PIN.";
      }
    }).catch(function () { $("pairErr").textContent = "Cannot reach the console."; });
  };
  $("pin").addEventListener("keydown", function (e) { if (e.key === "Enter") $("pairBtn").onclick(); });

  // Commands are batched so a fast trackpad sends one request at a time.
  var queue = [], inflight = false;
  function send(cmd) {
    var last = queue[queue.length - 1];
    if (last && (cmd.type === "pointer" || cmd.type === "scroll") && last.type === cmd.type) {
      last.dx += cmd.dx; last.dy += cmd.dy;
    } else {
      queue.push(cmd);
    }
    flush();
  }
  function flush() {
    if (inflight || !queue.length) return;
    inflight = true;
    var batch = queue; queue = [];
    api("POST", "/api/command", batch).then(function (j) {
      inflight = false;
      if (j.httpStatus === 401) { showPair("The TV forgot this device. Enter the new PIN."); return; }
      $("err").textContent = j.httpStatus === 200 ? "" : "Error: " + (j.error || j.httpStatus);
      flush();
    }).catch(function () { inflight = false; $("err").textContent = "Cannot reach the console."; });
  }
  function act(name) { send({ type: "action", name: name }); }

  function poll() {
    if (!token) return;
    api("GET", "/api/status").then(function (j) {
      if (j.httpStatus === 401) { showPair("The TV forgot this device. Enter the new PIN."); return; }
      if (j.httpStatus === 200) {
        var s = (j.title || j.url || "Connected");
        if (j.tabs) s = "Tab " + (j.active_tab + 1) + "/" + j.tabs + " · " + s;
        $("status").textContent = s;
        $("status").title = j.url || "";
        if (document.activeElement !== $("url") && j.url !== undefined) $("url").placeholder = j.url || "Address or search";
      }
      setTimeout(poll, 1500);
    }).catch(function () { setTimeout(poll, 3000); });
  }

  function openUrl(newTab) {
    var v = $("url").value.trim();
    if (!v) return;
    send({ type: "open", text: v, new_tab: newTab });
    $("url").value = ""; $("url").blur();
  }
  $("go").onclick = function () { openUrl(false); };
  $("goTab").onclick = function () { openUrl(true); };
  $("url").addEventListener("keydown", function (e) {
    if (e.key === "Enter") openUrl(e.ctrlKey || e.metaKey || e.altKey);
    else if (e.key === "Escape") $("url").blur();
  });

  function sendText(submit) {
    var v = $("text").value;
    if (v || submit) send({ type: "text", text: v, submit: submit });
    $("text").value = "";
  }
  $("send").onclick = function () { sendText(false); };
  $("enter").onclick = function () { sendText(true); };
  $("text").addEventListener("keydown", function (e) {
    if (e.key === "Enter") sendText(true);
    else if (e.key === "Escape") $("text").blur();
  });

  Array.prototype.forEach.call(document.querySelectorAll("[data-a]"), function (b) {
    b.onclick = function () { act(b.getAttribute("data-a")); b.blur(); };
  });

  // Trackpad. Pointer events cover touch, pen and mouse. Fingers get
  // velocity-based acceleration so one swipe crosses a 1920-px screen; a
  // mouse is precise already and moves at a steady 2x.
  var pad = $("pad"), touches = {}, count = 0, moved = 0, downAt = 0, scrolled = false, kind = "touch";
  function gain(d) {
    if (kind === "mouse") return d * 2;
    var a = Math.abs(d);
    return d * (a > 12 ? 3.5 : a > 4 ? 2.4 : 1.6);
  }
  pad.addEventListener("pointerdown", function (e) {
    if (locked()) return;
    if (e.pointerType === "mouse" && e.button !== 0) return;
    try { pad.setPointerCapture(e.pointerId); } catch (err) {}
    touches[e.pointerId] = { x: e.clientX, y: e.clientY };
    count++;
    if (count === 1) { moved = 0; downAt = Date.now(); scrolled = false; kind = e.pointerType || "touch"; }
    e.preventDefault();
  });
  pad.addEventListener("pointermove", function (e) {
    var t = touches[e.pointerId];
    if (!t || locked()) return;
    var dx = e.clientX - t.x, dy = e.clientY - t.y;
    t.x = e.clientX; t.y = e.clientY;
    moved += Math.abs(dx) + Math.abs(dy);
    if (count >= 2) {
      scrolled = true;
      send({ type: "scroll", dx: -dx * 3 / count, dy: -dy * 3 / count });
    } else {
      send({ type: "pointer", dx: gain(dx), dy: gain(dy) });
    }
    e.preventDefault();
  });
  function up(e) {
    if (!touches[e.pointerId]) return;
    delete touches[e.pointerId];
    count = Math.max(0, count - 1);
    var quick = kind === "mouse" || Date.now() - downAt < 300;  // a mouse click may be slow
    if (count === 0 && !scrolled && moved < 10 && quick && e.type === "pointerup") send({ type: "click" });
  }
  pad.addEventListener("pointerup", up);
  pad.addEventListener("pointercancel", up);
  pad.addEventListener("contextmenu", function (e) { e.preventDefault(); });

  // Mouse wheel and two-finger trackpad scrolling on a computer.
  pad.addEventListener("wheel", function (e) {
    var k = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 800 : 1;
    send({ type: "scroll", dx: e.deltaX * k, dy: e.deltaY * k });
    e.preventDefault();
  }, { passive: false });

  // Captured mouse: the computer's mouse becomes the TV pointer.
  function locked() { return document.pointerLockElement === pad; }
  $("lock").onclick = function () {
    if (pad.requestPointerLock) pad.requestPointerLock();
  };
  document.addEventListener("pointerlockchange", function () {
    var on = locked();
    pad.classList.toggle("locked", on);
    $("padHint").textContent = on ? "Mouse captured: move, click, scroll · Esc releases it"
                                  : "Drag with the mouse to move · click to click · wheel to scroll";
    $("lock").textContent = on ? "Release (Esc)" : "Capture mouse";
  });
  document.addEventListener("mousemove", function (e) {
    if (locked() && (e.movementX || e.movementY)) send({ type: "pointer", dx: e.movementX * 1.5, dy: e.movementY * 1.5 });
  });
  document.addEventListener("mousedown", function (e) {
    if (!locked()) return;
    if (e.button === 0) send({ type: "click" });
    else if (e.button === 2) act("escape");
    else if (e.button === 3) act("back");
    else if (e.button === 4) act("forward");
    e.preventDefault();
  });

  // Keyboard shortcuts, while no text box has the focus.
  var keymap = {
    "ArrowUp": function () { send({ type: "scroll", dx: 0, dy: -120 }); },
    "ArrowDown": function () { send({ type: "scroll", dx: 0, dy: 120 }); },
    "ArrowLeft": function () { send({ type: "scroll", dx: -120, dy: 0 }); },
    "ArrowRight": function () { send({ type: "scroll", dx: 120, dy: 0 }); },
    "PageUp": function () { act("page_up"); },
    "PageDown": function () { act("page_down"); },
    "Backspace": function () { act("back"); },
    "BrowserBack": function () { act("back"); },
    "BrowserForward": function () { act("forward"); },
    "Escape": function () { act("escape"); },
    "Enter": function () { send({ type: "click" }); },
    "[": function () { act("prev_tab"); },
    "]": function () { act("next_tab"); },
    "m": function () { act("menu"); },
    "f": function () { act("fullscreen"); },
    "/": function () { $("url").focus(); },
    "i": function () { $("text").focus(); }
  };
  document.addEventListener("keydown", function (e) {
    if (!token || !$("remote").classList.contains("on")) return;
    var tag = (document.activeElement && document.activeElement.tagName) || "";
    if (tag === "INPUT" || tag === "TEXTAREA") return;
    if (tag === "BUTTON" && (e.key === "Enter" || e.key === " ")) return;  // let buttons work
    if (e.ctrlKey || e.metaKey) return;
    var fn;
    if (e.altKey && e.key === "ArrowLeft") fn = function () { act("back"); };
    else if (e.altKey && e.key === "ArrowRight") fn = function () { act("forward"); };
    else if (e.altKey) return;
    else if (e.key === " ") fn = function () { act(e.shiftKey ? "page_up" : "page_down"); };
    else if (e.key === "Escape" && locked()) return;  // the browser releases the mouse
    else fn = keymap[e.key.length === 1 ? e.key.toLowerCase() : e.key];
    if (!fn) return;
    e.preventDefault();
    fn();
  });

  if (token) showRemote(); else showPair("");
})();
</script>
</body>
</html>
)PWHTML";
  return page;
}

}  // namespace prosperoweb::remote
