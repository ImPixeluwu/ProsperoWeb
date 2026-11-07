// SPDX-License-Identifier: GPL-3.0-or-later
//
// The page phones load from the remote control: PIN entry, then a trackpad
// (drag moves the pointer, tap clicks, two fingers scroll), an address bar,
// a text box for the focused field and the browser's buttons. Self-contained
// (no external resources), so it works without Internet access.
#include "prosperoweb/remote/remote_api.h"

namespace prosperoweb::remote {

const std::string& phone_page() {
  static const std::string page = R"PWHTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<meta name="color-scheme" content="dark">
<title>ProsperoWeb remote</title>
<style>
:root { --bg:#101218; --panel:#1b1e27; --line:#2c3040; --text:#e8eaf0; --dim:#9aa0b2; --accent:#4c8dff; --warn:#e0a83a; --bad:#ff6b6b; }
* { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
html, body { margin:0; height:100%; background:var(--bg); color:var(--text);
  font:16px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
body { display:flex; flex-direction:column; padding:12px 16px calc(12px + env(safe-area-inset-bottom)); gap:10px; }
h1 { font-size:18px; margin:0; font-weight:600; }
.row { display:flex; gap:8px; }
input, button { font:inherit; color:inherit; border-radius:10px; border:1px solid var(--line); background:var(--panel); }
input { flex:1; min-width:0; padding:10px 12px; }
button { padding:10px 12px; cursor:pointer; touch-action:manipulation; }
button:active { background:var(--line); }
button.primary { background:var(--accent); border-color:var(--accent); color:#fff; }
.note { color:var(--dim); font-size:13px; margin:0; }
.warn { color:var(--warn); font-size:13px; margin:0; }
.err { color:var(--bad); font-size:14px; min-height:1.4em; margin:0; }
#pair { display:flex; flex-direction:column; gap:12px; margin-top:12vh; }
#pin { font-size:28px; letter-spacing:8px; text-align:center; }
#remote { display:none; flex:1; flex-direction:column; gap:10px; min-height:0; }
#pad { flex:1; min-height:200px; border:1px solid var(--line); border-radius:14px; background:var(--panel);
  touch-action:none; display:flex; align-items:center; justify-content:center; color:var(--dim); user-select:none; -webkit-user-select:none; }
.grid { display:grid; grid-template-columns:repeat(4, 1fr); gap:8px; }
#status { color:var(--dim); font-size:13px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; margin:0; }
</style>
</head>
<body>
<h1>ProsperoWeb remote</h1>

<section id="pair">
  <p class="note">Enter the PIN shown on the TV.</p>
  <input id="pin" inputmode="numeric" autocomplete="one-time-code" maxlength="6" placeholder="000000">
  <button id="pairBtn" class="primary">Connect</button>
  <p id="pairErr" class="err"></p>
  <p class="warn">The connection is not encrypted: anyone on this Wi-Fi network could read what you type. Avoid typing passwords here.</p>
</section>

<section id="remote">
  <p id="status">Connected</p>
  <div class="row">
    <input id="url" type="url" inputmode="url" autocapitalize="off" autocomplete="off" placeholder="Address or search">
    <button id="go" class="primary">Go</button>
    <button id="goTab">+Tab</button>
  </div>
  <div id="pad">Drag to move &middot; tap to click &middot; two fingers to scroll</div>
  <div class="grid">
    <button data-a="back">&larr;</button>
    <button data-a="forward">&rarr;</button>
    <button data-a="reload">&#x21bb;</button>
    <button data-a="escape">Esc</button>
    <button data-a="prev_tab">Tab&lt;</button>
    <button data-a="next_tab">Tab&gt;</button>
    <button data-a="new_tab">New</button>
    <button data-a="close_tab">Close</button>
    <button data-a="page_up">PgUp</button>
    <button data-a="page_down">PgDn</button>
    <button data-a="fullscreen">Full</button>
    <button data-a="menu">Menu</button>
  </div>
  <div class="row">
    <input id="text" autocomplete="off" placeholder="Type into the focused field">
    <button id="send">Send</button>
    <button id="enter">&#x23ce;</button>
  </div>
  <p id="err" class="err"></p>
</section>

<script>
(function () {
  "use strict";
  var KEY = "pw-remote-token";
  var token = "";
  try { token = localStorage.getItem(KEY) || ""; } catch (e) {}
  var $ = function (id) { return document.getElementById(id); };

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
    $("remote").style.display = "none";
    $("pair").style.display = "flex";
    $("pairErr").textContent = message || "";
  }
  function showRemote() {
    $("pair").style.display = "none";
    $("remote").style.display = "flex";
    poll();
  }

  $("pairBtn").onclick = function () {
    api("POST", "/api/pair", { pin: $("pin").value.trim() }).then(function (j) {
      if (j.token) {
        token = j.token;
        try { localStorage.setItem(KEY, token); } catch (e) {}
        showRemote();
      } else if (j.httpStatus === 429) {
        $("pairErr").textContent = "Too many wrong PINs. Turn the remote control off and on again on the TV.";
      } else {
        $("pairErr").textContent = "Wrong PIN.";
      }
    }).catch(function () { $("pairErr").textContent = "Cannot reach the console."; });
  };
  $("pin").addEventListener("keydown", function (e) { if (e.key === "Enter") $("pairBtn").onclick(); });

  // Commands are batched so a fast trackpad sends one request per frame.
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
      if (j.httpStatus === 401) { showPair("The TV forgot this phone. Enter the new PIN."); return; }
      $("err").textContent = j.httpStatus === 200 ? "" : "Error: " + (j.error || j.httpStatus);
      flush();
    }).catch(function () { inflight = false; $("err").textContent = "Cannot reach the console."; });
  }

  function poll() {
    if (!token) return;
    api("GET", "/api/status").then(function (j) {
      if (j.httpStatus === 401) { showPair("The TV forgot this phone. Enter the new PIN."); return; }
      if (j.httpStatus === 200) {
        var s = (j.title || j.url || "Connected");
        if (j.tabs) s = "Tab " + (j.active_tab + 1) + "/" + j.tabs + " · " + s;
        $("status").textContent = s;
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
  $("url").addEventListener("keydown", function (e) { if (e.key === "Enter") openUrl(false); });

  function sendText(submit) {
    var v = $("text").value;
    if (v || submit) send({ type: "text", text: v, submit: submit });
    $("text").value = "";
  }
  $("send").onclick = function () { sendText(false); };
  $("enter").onclick = function () { sendText(true); };
  $("text").addEventListener("keydown", function (e) { if (e.key === "Enter") sendText(true); });

  Array.prototype.forEach.call(document.querySelectorAll("[data-a]"), function (b) {
    b.onclick = function () { send({ type: "action", name: b.getAttribute("data-a") }); };
  });

  // Trackpad. Pointer events cover touch and mouse; speed grows with the
  // finger's velocity so the pointer can cross a 1920-px screen in one swipe.
  var pad = $("pad"), touches = {}, count = 0, moved = 0, downAt = 0, scrolled = false;
  function gain(d) { var a = Math.abs(d); return d * (a > 12 ? 3.5 : a > 4 ? 2.4 : 1.6); }
  pad.addEventListener("pointerdown", function (e) {
    pad.setPointerCapture(e.pointerId);
    touches[e.pointerId] = { x: e.clientX, y: e.clientY };
    count++;
    if (count === 1) { moved = 0; downAt = Date.now(); scrolled = false; }
    e.preventDefault();
  });
  pad.addEventListener("pointermove", function (e) {
    var t = touches[e.pointerId];
    if (!t) return;
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
    if (count === 0 && !scrolled && moved < 10 && Date.now() - downAt < 300) send({ type: "click" });
  }
  pad.addEventListener("pointerup", up);
  pad.addEventListener("pointercancel", up);

  if (token) showRemote(); else showPair("");
})();
</script>
</body>
</html>
)PWHTML";
  return page;
}

}  // namespace prosperoweb::remote
