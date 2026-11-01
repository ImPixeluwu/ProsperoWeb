// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProsperoWeb YouTube video-ad userscript (document-start, MAIN/page world).
//
// General web ads, trackers and page-ad cosmetic hiding are handled by the
// native WebKit content blocker (WKContentRuleList), which never touches the
// page's JavaScript. This userscript does ONLY the one thing the content
// blocker cannot: remove YouTube's client-scheduled pre/mid-roll video ads by
// pruning adPlacements/playerAds/adSlots from the player response (the uAssets
// json-prune technique), plus best-effort skip/seek for anything left.
//
// DESIGN RULE: strictly fail-open. The JSON.parse proxy and the
// ytInitialPlayerResponse setter must NEVER throw into the page and must
// always return the original data on any unexpected shape or error, so this
// script can only ever remove ads, never break YouTube (or any other site).
//
// The UI process prepends: window.__PROSPEROWEB_AB = { enabled: true };
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
  function log(msg) {
    try { console.log('[adblock-yt] ' + msg); } catch (e) {}
  }

  var pruneFired = false;

  // Surgical: only delete the three ad fields, only when present, in place.
  // Never walks or rewrites unrelated JSON. Swallows every error (fail-open).
  function prunePlayer(o) {
    try {
      if (!o || typeof o !== 'object') { return; }
      var keys = ['adPlacements', 'playerAds', 'adSlots'];
      for (var i = 0; i < keys.length; i++) {
        if (Object.prototype.hasOwnProperty.call(o, keys[i])) { delete o[keys[i]]; pruneFired = true; }
      }
      var pr = o.playerResponse;
      if (pr && typeof pr === 'object') {
        for (var j = 0; j < keys.length; j++) {
          if (Object.prototype.hasOwnProperty.call(pr, keys[j])) { delete pr[keys[j]]; pruneFired = true; }
        }
      }
    } catch (e) {}
  }

  // Proxy JSON.parse (covers the player response fetched on SPA navigation).
  try {
    var _parse = JSON.parse;
    JSON.parse = function () {
      var out = _parse.apply(this, arguments);  // native first; untouched on error
      if (enabled()) { prunePlayer(out); }
      return out;
    };
  } catch (e) {}

  // The initial watch page assigns ytInitialPlayerResponse directly (not via
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

  // Best-effort for anything the prune misses (server-stitched ads): click the
  // skip button, close overlays, and fast-forward + mute an ad that is showing.
  function tick() {
    try {
      if (!enabled() || !isYouTube()) { return; }
      var skip = document.querySelector(
          '.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, ' +
          '.ytp-ad-skip-button-container button');
      if (skip) { skip.click(); }
      var closers = document.querySelectorAll('.ytp-ad-overlay-close-button');
      for (var i = 0; i < closers.length; i++) { closers[i].click(); }
      if (document.querySelector('.ad-showing, .ad-interrupting')) {
        var v = document.querySelector('video');
        if (v) {
          if (isFinite(v.duration) && v.duration > 0) { v.currentTime = v.duration; }
          v.muted = true;
        }
      }
    } catch (e) {}
  }

  try {
    setInterval(tick, 500);
    setInterval(function () {
      if (pruneFired) { log('pruned YouTube ad placements on ' + location.host); pruneFired = false; }
    }, 5000);
    log('installed on ' + location.host + ' (enabled=' + enabled() + ')');
  } catch (e) {
    log('init error (failing open): ' + (e && e.message));
  }
})();
