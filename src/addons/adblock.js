// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProsperoWeb bundled ad-block userscript (document-start, MAIN/page world).
//
// Our WebKit build has ENABLE(CONTENT_EXTENSIONS) off, so the native content
// rule list is unusable. This userscript enforces the subset of EasyList/
// EasyPrivacy/uBlock/Peter-Lowe filters a page-world script can:
//   - whole-host network blocking (||host^) via fetch/XHR/WebSocket/sendBeacon
//     wrappers + a MutationObserver that strips ad script/iframe/img nodes;
//   - generic + domain-scoped element-hiding CSS (hides page ads incl. YouTube
//     promoted/masthead/sidebar);
//   - YouTube video-ad removal via a SURGICAL json-prune of adPlacements /
//     playerAds / adSlots (the uAssets technique).
//
// DESIGN RULE: fail-open. Every hook is wrapped so it can never throw into the
// page and always falls through to the original behaviour / original data on
// any error or unexpected shape. The ad blocker must degrade to "does nothing"
// rather than ever break a site.
(function () {
  'use strict';

  var cfg = window.__PROSPEROWEB_AB || {};
  window.__PROSPEROWEB_AB = cfg;
  if (window.__prosperowebAbInstalled) {
    return;
  }
  window.__prosperowebAbInstalled = true;

  function enabled() {
    return !!(window.__PROSPEROWEB_AB && window.__PROSPEROWEB_AB.enabled);
  }

  var blockedCount = 0;
  var pruneFired = false;

  function log(msg) {
    try { console.log('[adblock] ' + msg); } catch (e) {}
  }

  // First-party / essential hosts never blocked, whatever the lists say
  // (YouTube app + media + thumbnails, so playback and navigation work).
  var FIRST_PARTY = ['youtube.com', 'youtube-nocookie.com', 'youtubekids.com',
                     'googlevideo.com', 'ytimg.com', 'ggpht.com'];
  function isFirstParty(host) {
    for (var i = 0; i < FIRST_PARTY.length; i++) {
      var d = FIRST_PARTY[i];
      if (host === d || (host.length > d.length && host.slice(-(d.length + 1)) === '.' + d)) {
        return true;
      }
    }
    return false;
  }

  var hostSet = Object.create(null);
  function rebuildHosts() {
    hostSet = Object.create(null);
    try {
      var h = (window.__PROSPEROWEB_AB && window.__PROSPEROWEB_AB.hosts) || [];
      for (var i = 0; i < h.length; i++) { hostSet[h[i]] = true; }
    } catch (e) {}
  }
  rebuildHosts();

  // Only AD / telemetry paths are blocked even on first-party hosts; nothing
  // playback-related (no qoe / watchtime).
  var BLOCK_PATHS = ['/pagead/', '/ptracking', '/api/stats/ads', '/api/stats/atr',
                     '/youtubei/v1/log_event'];

  function blocked(url) {
    try {
      if (!enabled() || !url) { return false; }
      var u = new URL(url, location.href);
      var path = u.pathname;
      for (var i = 0; i < BLOCK_PATHS.length; i++) {
        if (path.indexOf(BLOCK_PATHS[i]) !== -1) { return true; }
      }
      var h = u.hostname;
      if (isFirstParty(h)) { return false; }  // never host-block essentials
      while (h) {
        if (hostSet[h]) { return true; }
        var dot = h.indexOf('.');
        if (dot < 0) { break; }
        h = h.slice(dot + 1);
      }
    } catch (e) {}
    return false;
  }

  // --- Network blocking (all frames), each hook fail-open -------------------
  try {
    var of = window.fetch;
    if (of) {
      window.fetch = function (input, init) {
        try {
          var u = (typeof input === 'string') ? input : (input && input.url);
          if (blocked(u)) {
            blockedCount++;
            return Promise.resolve(new Response('', { status: 204, statusText: 'No Content' }));
          }
        } catch (e) {}
        return of.apply(this, arguments);
      };
    }
  } catch (e) {}
  try {
    var xhrOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
      try { this.__pwBlocked = blocked(url); } catch (e) { this.__pwBlocked = false; }
      return xhrOpen.apply(this, arguments);
    };
    var xhrSend = XMLHttpRequest.prototype.send;
    XMLHttpRequest.prototype.send = function () {
      if (this.__pwBlocked) { blockedCount++; return; }
      return xhrSend.apply(this, arguments);
    };
  } catch (e) {}
  try {
    if (navigator.sendBeacon) {
      var ob = navigator.sendBeacon.bind(navigator);
      navigator.sendBeacon = function (url, data) {
        try { if (blocked(url)) { blockedCount++; return true; } } catch (e) {}
        return ob(url, data);
      };
    }
  } catch (e) {}
  try {
    var OWS = window.WebSocket;
    if (OWS) {
      window.WebSocket = function (url, protocols) {
        try { if (blocked(url)) { blockedCount++; return { close: function () {} }; } } catch (e) {}
        return protocols === undefined ? new OWS(url) : new OWS(url, protocols);
      };
      window.WebSocket.prototype = OWS.prototype;
    }
  } catch (e) {}

  function stripNode(el) {
    try {
      if (!el || el.nodeType !== 1) { return; }
      var src = el.getAttribute && el.getAttribute('src');
      if (src && blocked(src)) { el.remove(); blockedCount++; return; }
      if (el.querySelectorAll) {
        var kids = el.querySelectorAll('script[src],iframe[src],img[src]');
        for (var i = 0; i < kids.length; i++) {
          var s = kids[i].getAttribute('src');
          if (s && blocked(s)) { kids[i].remove(); blockedCount++; }
        }
      }
    } catch (e) {}
  }

  // --- YouTube video-ad removal: SURGICAL json-prune ------------------------
  // Only ever deletes adPlacements/playerAds/adSlots, and only when they are
  // present. Never walks or rewrites unrelated JSON. Mutates in place; returns
  // nothing. Any error is swallowed (fail-open).
  function prunePlayer(o) {
    try {
      if (!o || typeof o !== 'object') { return; }
      var keys = ['adPlacements', 'playerAds', 'adSlots'];
      var hit = false;
      for (var i = 0; i < keys.length; i++) {
        if (Object.prototype.hasOwnProperty.call(o, keys[i])) { delete o[keys[i]]; hit = true; }
      }
      var pr = o.playerResponse;
      if (pr && typeof pr === 'object') {
        for (var j = 0; j < keys.length; j++) {
          if (Object.prototype.hasOwnProperty.call(pr, keys[j])) { delete pr[keys[j]]; hit = true; }
        }
      }
      if (hit) { pruneFired = true; }
    } catch (e) {}
  }

  try {
    var _parse = JSON.parse;
    JSON.parse = function (text, reviver) {
      var out = _parse.apply(this, arguments);  // native first; never altered on error
      if (enabled()) { prunePlayer(out); }       // surgical, guarded, in place
      return out;
    };
  } catch (e) {}

  // The inline ytInitialPlayerResponse is assigned directly (not via
  // JSON.parse); prune it on set. Always stores the value, pruned or not.
  try {
    var _ypr;
    Object.defineProperty(window, 'ytInitialPlayerResponse', {
      configurable: true,
      get: function () { return _ypr; },
      set: function (v) { if (enabled()) { prunePlayer(v); } _ypr = v; }
    });
  } catch (e) {}

  function isYouTube() {
    try {
      var h = location.hostname.replace(/^www\./, '').replace(/^m\./, '');
      return h === 'youtube.com' || h === 'youtube-nocookie.com' || h === 'youtubekids.com';
    } catch (e) { return false; }
  }
  function youtubeTick() {
    try {
      if (!enabled() || !isYouTube()) { return; }
      var skip = document.querySelector(
          '.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, ' +
          '.ytp-ad-skip-button-container button');
      if (skip) { skip.click(); }
      var closeOverlay = document.querySelectorAll('.ytp-ad-overlay-close-button');
      for (var i = 0; i < closeOverlay.length; i++) { closeOverlay[i].click(); }
      if (document.querySelector('.ad-showing, .ad-interrupting')) {
        var v = document.querySelector('video');
        if (v) {
          if (isFinite(v.duration) && v.duration > 0) { v.currentTime = v.duration; }
          v.muted = true;
        }
      }
    } catch (e) {}
  }

  // --- Cosmetic hiding (top frame only) -------------------------------------
  var styleEl = null;
  function cosmeticCss() {
    var css = '';
    try {
      css = (window.__PROSPEROWEB_AB && window.__PROSPEROWEB_AB.css) || '';
      var rules = (window.__PROSPEROWEB_AB && window.__PROSPEROWEB_AB.domainRules) || [];
      var host = location.hostname;
      var sels = [];
      for (var i = 0; i < rules.length; i++) {
        var d = rules[i][0];
        if (host === d || (host.length > d.length && host.slice(-(d.length + 1)) === '.' + d)) {
          sels.push(rules[i][1]);
        }
      }
      if (isYouTube()) {
        sels.push('ytd-ad-slot-renderer', 'ytd-promoted-sparkles-web-renderer',
                  'ytd-promoted-video-renderer', 'ytd-display-ad-renderer',
                  'ytd-in-feed-ad-layout-renderer', '#player-ads', '#masthead-ad',
                  '.ytp-ad-module', 'ytd-ad-inline-playback-meta-block');
      }
      if (sels.length) {
        css += (css ? '\n' : '') + sels.join(',') + '{display:none!important}';
      }
    } catch (e) { return ''; }
    return css;
  }
  function applyCosmetic() {
    try {
      if (window.top !== window.self) { return; }  // top frame only (memory)
      if (!enabled()) { if (styleEl) { styleEl.textContent = ''; } return; }
      var css = cosmeticCss();
      if (!css) { return; }
      if (!styleEl) {
        styleEl = document.createElement('style');
        styleEl.setAttribute('data-prosperoweb-adblock', '1');
      }
      styleEl.textContent = css;
      var root = document.head || document.documentElement;
      if (root && styleEl.parentNode !== root) { root.appendChild(styleEl); }
    } catch (e) {}
  }

  var observer = null;
  try {
    observer = new MutationObserver(function (muts) {
      for (var i = 0; i < muts.length; i++) {
        var added = muts[i].addedNodes;
        for (var j = 0; j < added.length; j++) { stripNode(added[j]); }
      }
    });
  } catch (e) {}

  window.__PROSPEROWEB_AB.apply = function () {
    rebuildHosts();
    applyCosmetic();
  };

  try {
    applyCosmetic();
    if (observer && document.documentElement) {
      observer.observe(document.documentElement, { childList: true, subtree: true });
    }
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', applyCosmetic, true);
    }
    document.addEventListener('yt-navigate-finish', applyCosmetic, true);
    setInterval(youtubeTick, 500);
    setInterval(function () {
      if (blockedCount > 0 || pruneFired) {
        log('page ' + location.host + ': blocked ' + blockedCount + ' requests, json-prune ' +
            (pruneFired ? 'fired' : 'idle'));
        blockedCount = 0;
      }
    }, 5000);
    log('installed on ' + location.host + ' (enabled=' + enabled() + ')');
  } catch (e) {
    log('init error (failing open): ' + (e && e.message));
  }
})();
