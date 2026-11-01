// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProsperoWeb bundled ad-block userscript (document-start, MAIN/page world).
//
// Our WebKit build has ENABLE(CONTENT_EXTENSIONS) off, so the native content
// rule list is unusable. This userscript enforces the subset of EasyList/
// EasyPrivacy/uBlock/Peter-Lowe filters that a page-world script can, per
// docs/webkit-adblock.md:
//   - whole-host network blocking (||host^ rules) by wrapping fetch/XHR/
//     WebSocket/sendBeacon and stripping ad <script>/<iframe>/<img> nodes;
//   - generic + domain-scoped element-hiding CSS (hides page ads incl. YouTube
//     promoted/masthead/sidebar);
//   - YouTube video-ad removal: a JSON.parse / ytInitialPlayerResponse proxy
//     that prunes adPlacements/playerAds/adSlots (the uAssets json-prune trick,
//     the only thing that kills client-scheduled pre/mid-roll), plus telemetry
//     blocking and skip/seek fallbacks.
// It is NOT uBlock Origin: no scriptlets, no procedural cosmetics. Best-effort.
//
// The UI process prepends a config object and may call apply() live:
//   window.__PROSPEROWEB_AB = { enabled, hosts:[...], css:"...",
//                               domainRules:[["domain","selector"],...] };
(function () {
  'use strict';

  var cfg = window.__PROSPEROWEB_AB || {};
  window.__PROSPEROWEB_AB = cfg;
  if (window.__prosperowebAbInstalled) {
    return;
  }
  window.__prosperowebAbInstalled = true;

  var blockedCount = 0;

  function hostOf(url) {
    try {
      return new URL(url, location.href).hostname;
    } catch (e) {
      return '';
    }
  }

  var hostSet = Object.create(null);
  function rebuildHosts() {
    hostSet = Object.create(null);
    var h = window.__PROSPEROWEB_AB.hosts || [];
    for (var i = 0; i < h.length; i++) {
      hostSet[h[i]] = true;
    }
  }
  rebuildHosts();

  // YouTube ad-telemetry / ad endpoints to drop (docs step 3). Pathname match.
  var BLOCK_PATHS = [
    '/pagead/', '/ptracking', '/api/stats/ads', '/api/stats/atr', '/api/stats/qoe',
    '/youtubei/v1/log_event', '/get_midroll_', '/api/stats/watchtime'
  ];
  function pathBlocked(url) {
    try {
      var p = new URL(url, location.href).pathname;
      for (var i = 0; i < BLOCK_PATHS.length; i++) {
        if (p.indexOf(BLOCK_PATHS[i]) !== -1) {
          return true;
        }
      }
    } catch (e) {}
    return false;
  }

  function blocked(url) {
    if (!window.__PROSPEROWEB_AB.enabled || !url) {
      return false;
    }
    if (pathBlocked(url)) {
      return true;
    }
    var h = hostOf(url);
    while (h) {
      if (hostSet[h]) {
        return true;
      }
      var dot = h.indexOf('.');
      if (dot < 0) {
        break;
      }
      h = h.slice(dot + 1);
    }
    return false;
  }

  // --- Network blocking (all frames) ----------------------------------------
  var of = window.fetch;
  if (of) {
    window.fetch = function (input, init) {
      var u = (typeof input === 'string') ? input : (input && input.url);
      if (blocked(u)) {
        blockedCount++;
        return Promise.resolve(new Response('', { status: 204, statusText: 'No Content' }));
      }
      return of.apply(this, arguments);
    };
  }
  var xhrOpen = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (method, url) {
    this.__pwBlocked = blocked(url);
    return xhrOpen.apply(this, arguments);
  };
  var xhrSend = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.send = function () {
    if (this.__pwBlocked) {
      blockedCount++;
      return;  // never dispatched
    }
    return xhrSend.apply(this, arguments);
  };
  if (navigator.sendBeacon) {
    var ob = navigator.sendBeacon.bind(navigator);
    navigator.sendBeacon = function (url, data) {
      if (blocked(url)) { blockedCount++; return true; }
      return ob(url, data);
    };
  }
  var OWS = window.WebSocket;
  if (OWS) {
    window.WebSocket = function (url, protocols) {
      if (blocked(url)) {
        blockedCount++;
        throw new Error('blocked by ProsperoWeb ad blocker');
      }
      return protocols === undefined ? new OWS(url) : new OWS(url, protocols);
    };
    window.WebSocket.prototype = OWS.prototype;
  }
  function stripNode(el) {
    if (el.nodeType !== 1) { return; }
    var src = el.getAttribute && el.getAttribute('src');
    if (src && blocked(src)) { el.remove(); blockedCount++; return; }
    if (el.querySelectorAll) {
      var kids = el.querySelectorAll('script[src],iframe[src],img[src]');
      for (var i = 0; i < kids.length; i++) {
        var s = kids[i].getAttribute('src');
        if (s && blocked(s)) { kids[i].remove(); blockedCount++; }
      }
    }
  }

  // --- YouTube video-ad removal (json-prune, MAIN world, document-start) -----
  function pruneObject(o) {
    if (!o || typeof o !== 'object') { return o; }
    var keys = ['adPlacements', 'playerAds', 'adSlots'];
    for (var i = 0; i < keys.length; i++) {
      if (o[keys[i]]) { delete o[keys[i]]; }
      if (o.playerResponse && o.playerResponse[keys[i]]) { delete o.playerResponse[keys[i]]; }
    }
    // Feed ads: contents[...].richItemRenderer.content.adSlotRenderer and
    // generic adSlotRenderer / promoted entries are dropped where found.
    try {
      pruneFeed(o);
    } catch (e) {}
    return o;
  }
  function pruneFeed(node, depth) {
    depth = depth || 0;
    if (!node || typeof node !== 'object' || depth > 8) { return; }
    if (Array.isArray(node)) {
      for (var i = node.length - 1; i >= 0; i--) {
        var it = node[i];
        if (it && typeof it === 'object' &&
            (it.adSlotRenderer || it.promotedVideoRenderer || it.compactPromotedVideoRenderer ||
             (it.richItemRenderer && it.richItemRenderer.content &&
              it.richItemRenderer.content.adSlotRenderer))) {
          node.splice(i, 1);
        } else {
          pruneFeed(it, depth + 1);
        }
      }
      return;
    }
    for (var k in node) {
      if (Object.prototype.hasOwnProperty.call(node, k)) {
        pruneFeed(node[k], depth + 1);
      }
    }
  }

  function installYouTubePruning() {
    // 1. Proxy JSON.parse before base.js runs.
    var _parse = JSON.parse;
    JSON.parse = function () {
      var out = _parse.apply(this, arguments);
      return window.__PROSPEROWEB_AB.enabled ? pruneObject(out) : out;
    };
    // 2. Prune the inline ytInitialPlayerResponse / ytInitialData on set.
    function guard(name) {
      var store;
      try {
        Object.defineProperty(window, name, {
          configurable: true,
          get: function () { return store; },
          set: function (v) { store = window.__PROSPEROWEB_AB.enabled ? pruneObject(v) : v; }
        });
      } catch (e) {}
    }
    guard('ytInitialPlayerResponse');
    guard('ytInitialData');
  }

  function isYouTube() {
    var h = location.hostname.replace(/^www\./, '').replace(/^m\./, '');
    return h === 'youtube.com' || h === 'youtube-nocookie.com' || h === 'youtubekids.com';
  }
  function youtubeTick() {
    if (!window.__PROSPEROWEB_AB.enabled || !isYouTube()) { return; }
    var skip = document.querySelector(
        '.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, ' +
        '.ytp-ad-skip-button-container button');
    if (skip) { skip.click(); }
    var closeOverlay = document.querySelectorAll('.ytp-ad-overlay-close-button');
    for (var i = 0; i < closeOverlay.length; i++) { closeOverlay[i].click(); }
    // Any ad still shown (incl. server-stitched): fast-forward and mute it.
    if (document.querySelector('.ad-showing, .ad-interrupting')) {
      var v = document.querySelector('video');
      if (v) {
        try {
          if (isFinite(v.duration) && v.duration > 0) { v.currentTime = v.duration; }
          v.muted = true;
        } catch (e) {}
      }
    }
  }

  // --- Cosmetic hiding (top frame only) -------------------------------------
  var styleEl = null;
  function cosmeticCss() {
    var css = window.__PROSPEROWEB_AB.css || '';
    var rules = window.__PROSPEROWEB_AB.domainRules || [];
    var host = location.hostname;
    var sels = [];
    for (var i = 0; i < rules.length; i++) {
      var d = rules[i][0];
      if (host === d || (host.length > d.length && host.slice(-(d.length + 1)) === '.' + d)) {
        sels.push(rules[i][1]);
      }
    }
    // Always hide the common YouTube page-ad containers.
    if (isYouTube()) {
      sels.push('ytd-ad-slot-renderer', 'ytd-promoted-sparkles-web-renderer',
                'ytd-promoted-video-renderer', 'ytd-display-ad-renderer',
                'ytd-in-feed-ad-layout-renderer', '#player-ads', '#masthead-ad',
                '.ytp-ad-module', 'ytd-ad-inline-playback-meta-block');
    }
    if (sels.length) {
      css += (css ? '\n' : '') + sels.join(',') + '{display:none!important}';
    }
    return css;
  }
  function applyCosmetic() {
    if (window.top !== window.self) { return; }  // top frame only (memory)
    if (!window.__PROSPEROWEB_AB.enabled) {
      if (styleEl) { styleEl.textContent = ''; }
      return;
    }
    var css = cosmeticCss();
    if (!css) { return; }
    if (!styleEl) {
      styleEl = document.createElement('style');
      styleEl.setAttribute('data-prosperoweb-adblock', '1');
    }
    styleEl.textContent = css;
    var root = document.head || document.documentElement;
    if (root && styleEl.parentNode !== root) {
      root.appendChild(styleEl);
    }
  }

  var observer = new MutationObserver(function (muts) {
    for (var i = 0; i < muts.length; i++) {
      var added = muts[i].addedNodes;
      for (var j = 0; j < added.length; j++) {
        stripNode(added[j]);
      }
    }
  });

  window.__PROSPEROWEB_AB.apply = function () {
    rebuildHosts();
    applyCosmetic();
  };

  // Install the YouTube pruning hooks immediately (before base.js), always, so
  // a live toggle-on still catches the next navigation's player response.
  installYouTubePruning();

  function start() {
    applyCosmetic();
    if (document.documentElement) {
      observer.observe(document.documentElement, { childList: true, subtree: true });
    }
  }
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', applyCosmetic, true);
  }
  start();
  document.addEventListener('yt-navigate-finish', applyCosmetic, true);
  setInterval(youtubeTick, 500);
  // Periodic blocked-count report (visible with PROSPEROWEB_CONSOLE_LOG=1).
  setInterval(function () {
    if (blockedCount > 0) {
      try { console.log('[adblock] blocked ' + blockedCount + ' requests on ' + location.host); }
      catch (e) {}
      blockedCount = 0;
    }
  }, 5000);
})();
