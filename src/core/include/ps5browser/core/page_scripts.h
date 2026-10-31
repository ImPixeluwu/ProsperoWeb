// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5browser::core {

// User scripts the frontend injects into web content. Both are meant to run
// at document start in every frame.

// Reports EME usage to the browser through the "ps5browser" script message
// handler as {type: 'eme', keySystem, origin}, then lets the original API
// run. requestMediaKeySystemAccess() for any key system other than
// org.w3.clearkey still calls the original implementation, but its promise
// is turned into a NotSupportedError rejection (an original rejection is
// propagated as is), so pages fall back cleanly instead of hanging on a CDM
// that does not exist.
constexpr const char* kEmeDetectorScript = R"JS((function () {
  'use strict';
  if (window.__ps5browserEmeHooked) {
    return;
  }
  try {
    Object.defineProperty(window, '__ps5browserEmeHooked', { value: true });
  } catch (e) {
    return;
  }

  function report(keySystem) {
    try {
      var handlers = window.webkit && window.webkit.messageHandlers;
      if (handlers && handlers.ps5browser) {
        handlers.ps5browser.postMessage({
          type: 'eme',
          keySystem: String(keySystem),
          origin: String(window.location.origin)
        });
      }
    } catch (e) {
      // Reporting must never break the page.
    }
  }

  function notSupported(keySystem) {
    return new DOMException(
      'Key system "' + keySystem + '" is not supported by this browser',
      'NotSupportedError');
  }

  try {
    var navProto = window.Navigator && window.Navigator.prototype;
    var originalRequest = navProto && navProto.requestMediaKeySystemAccess;
    if (typeof originalRequest === 'function') {
      Object.defineProperty(navProto, 'requestMediaKeySystemAccess', {
        configurable: true,
        writable: true,
        value: function requestMediaKeySystemAccess(keySystem, configurations) {
          report(keySystem);
          var promise;
          try {
            promise = originalRequest.apply(this, arguments);
          } catch (e) {
            return Promise.reject(e);
          }
          if (keySystem === 'org.w3.clearkey') {
            return promise;
          }
          return Promise.resolve(promise).then(function () {
            throw notSupported(keySystem);
          });
        }
      });
    }
  } catch (e) {
  }

  try {
    var mediaProto = window.HTMLMediaElement && window.HTMLMediaElement.prototype;
    var originalSetMediaKeys = mediaProto && mediaProto.setMediaKeys;
    if (typeof originalSetMediaKeys === 'function') {
      Object.defineProperty(mediaProto, 'setMediaKeys', {
        configurable: true,
        writable: true,
        value: function setMediaKeys(mediaKeys) {
          if (mediaKeys) {
            report('unknown');
          }
          return originalSetMediaKeys.apply(this, arguments);
        }
      });
    }
  } catch (e) {
  }
})();
)JS";

// Reports focus changes of editable elements through the "ps5browser" script
// message handler as {type: 'focus', editable}, so the shell can offer the
// on-screen keyboard (EngineViewClient::text_input_focused). Frontends route
// these messages to text_input_focused() instead of script_message().
constexpr const char* kFocusReporterScript = R"JS((function () {
  'use strict';
  function editable(e) {
    if (!e) return false;
    if (e.isContentEditable) return true;
    var tag = e.tagName;
    if (tag === 'TEXTAREA') return !e.readOnly && !e.disabled;
    if (tag === 'INPUT') {
      var type = (e.getAttribute('type') || 'text').toLowerCase();
      return ['text', 'search', 'email', 'url', 'tel', 'password', 'number'].indexOf(type) >= 0 &&
             !e.readOnly && !e.disabled;
    }
    return false;
  }
  function post(value) {
    try {
      window.webkit.messageHandlers.ps5browser.postMessage({ type: 'focus', editable: value });
    } catch (e) {
    }
  }
  document.addEventListener('focusin', function (ev) { post(editable(ev.target)); }, true);
  document.addEventListener('focusout', function () { post(false); }, true);
})();
)JS";

// Maps DualSense input to DOM keyboard events for pages that expect a
// keyboard (e.g. video players). Intentionally empty for now.
constexpr const char* kGamepadKeyBridgeScript = R"JS((function () {
  'use strict';
  // TODO: translate gamepad shortcuts into KeyboardEvents (space, arrows, f).
})();
)JS";

}  // namespace ps5browser::core
