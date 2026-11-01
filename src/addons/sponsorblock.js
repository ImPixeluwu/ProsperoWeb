// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProsperoWeb bundled SponsorBlock user script.
//
// This is NOT the SponsorBlock browser extension; it is an independent,
// self-contained user script that talks to the public SponsorBlock API
// (https://sponsor.ajay.app) and skips segments on YouTube watch pages by
// seeking the <video> element. The seek logic mirrors
// prosperoweb::core::sponsorblock::seek_target (src/core/sponsorblock.cpp).
//
// The UI process injects this at document start in every frame and prepends a
// config object:
//
//   window.__PROSPEROWEB_SB = { enabled: true, categories: ["sponsor", ...],
//                               api: "https://sponsor.ajay.app" };
//
// and calls window.__PROSPEROWEB_SB.apply(categories) when the enabled
// categories change live, so no re-injection is needed for a category toggle.
(function () {
  'use strict';

  var cfg = window.__PROSPEROWEB_SB || {};
  cfg.categories = Array.isArray(cfg.categories) ? cfg.categories : ['sponsor'];
  cfg.api = typeof cfg.api === 'string' ? cfg.api : 'https://sponsor.ajay.app';

  // Guard against double injection (SPA frames, re-entry).
  if (window.__prosperowebSbInstalled) {
    window.__PROSPEROWEB_SB = cfg;
    return;
  }
  window.__prosperowebSbInstalled = true;
  window.__PROSPEROWEB_SB = cfg;

  var EPSILON = 0.3;        // seconds of lead-in, mirrors the C++ model
  var segments = [];        // [{start, end, category}]
  var currentVideoId = null;
  var videoEl = null;

  function log(msg) {
    try { console.log('[sponsorblock] ' + msg); } catch (e) {}
  }

  function videoIdFromLocation() {
    try {
      var u = new URL(window.location.href);
      var host = u.hostname.replace(/^www\./, '').replace(/^m\./, '');
      if (host !== 'youtube.com' && host !== 'youtube-nocookie.com') {
        return null;
      }
      if (u.pathname === '/watch') {
        return u.searchParams.get('v');
      }
      var m = u.pathname.match(/^\/(?:embed|shorts|live)\/([^/?#]+)/);
      return m ? m[1] : null;
    } catch (e) {
      return null;
    }
  }

  // Mirror of prosperoweb::core::sponsorblock::seek_target.
  function seekTarget(t, segs) {
    var target = null;
    var progressed = true;
    while (progressed) {
      progressed = false;
      for (var i = 0; i < segs.length; i++) {
        var s = segs[i];
        if (!(s.end > s.start)) { continue; }
        if (t >= s.start - EPSILON && t < s.end - EPSILON) {
          if (s.end > t) {
            t = s.end;
            target = t;
            progressed = true;
          }
        }
      }
    }
    return target;
  }

  function enabledSegments() {
    var cats = window.__PROSPEROWEB_SB.categories || [];
    return segments.filter(function (s) { return cats.indexOf(s.category) !== -1; });
  }

  function onTimeUpdate() {
    if (!videoEl || !window.__PROSPEROWEB_SB.enabled) { return; }
    var segs = enabledSegments();
    if (!segs.length) { return; }
    var target = seekTarget(videoEl.currentTime, segs);
    if (target !== null && target > videoEl.currentTime + 0.05) {
      log('skip to ' + target.toFixed(1) + 's');
      videoEl.currentTime = target;
    }
  }

  function attachVideo() {
    var v = document.querySelector('video');
    if (v && v !== videoEl) {
      if (videoEl) { videoEl.removeEventListener('timeupdate', onTimeUpdate); }
      videoEl = v;
      videoEl.addEventListener('timeupdate', onTimeUpdate);
    }
  }

  function fetchSegments(id) {
    var cats = window.__PROSPEROWEB_SB.categories || [];
    if (!cats.length) { segments = []; return; }
    var url = cfg.api + '/api/skipSegments?videoID=' + encodeURIComponent(id) +
              '&categories=' + encodeURIComponent(JSON.stringify(cats));
    fetch(url, { credentials: 'omit' }).then(function (r) {
      if (!r.ok) { throw new Error('http ' + r.status); }
      return r.json();
    }).then(function (data) {
      if (!Array.isArray(data)) { return; }
      segments = data.map(function (d) {
        return { start: d.segment[0], end: d.segment[1], category: d.category };
      });
      log('loaded ' + segments.length + ' segments for ' + id);
      attachVideo();
      onTimeUpdate();
    }).catch(function (e) {
      // 404 means "no segments"; anything else is logged and ignored.
      segments = [];
    });
  }

  function refresh() {
    var id = videoIdFromLocation();
    if (!id) { segments = []; currentVideoId = null; return; }
    if (id !== currentVideoId) {
      currentVideoId = id;
      segments = [];
      fetchSegments(id);
    }
    attachVideo();
  }

  // Called by the UI process when the enabled categories change live.
  window.__PROSPEROWEB_SB.apply = function (categories) {
    if (Array.isArray(categories)) {
      window.__PROSPEROWEB_SB.categories = categories;
    }
    // Re-fetch with the new category set for the current video.
    currentVideoId = null;
    refresh();
  };

  // YouTube is a single-page app: it fires yt-navigate-finish and mutates the
  // URL without a full load. Watch both.
  document.addEventListener('yt-navigate-finish', refresh, true);
  window.addEventListener('popstate', refresh, true);
  setInterval(function () {
    var id = videoIdFromLocation();
    if (id !== currentVideoId) { refresh(); } else { attachVideo(); }
  }, 1000);

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', refresh, true);
  } else {
    refresh();
  }
})();
