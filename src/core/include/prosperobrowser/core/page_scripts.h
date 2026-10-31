// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace prosperobrowser::core {

// User scripts the frontend injects into web content. Both are meant to run
// at document start in every frame.

// Reports EME usage to the browser through the "prosperobrowser" script message
// handler as {type: 'eme', keySystem, origin}, then lets the original API
// run. requestMediaKeySystemAccess() for any key system other than
// org.w3.clearkey still calls the original implementation, but its promise
// is turned into a NotSupportedError rejection (an original rejection is
// propagated as is), so pages fall back cleanly instead of hanging on a CDM
// that does not exist.
constexpr const char* kEmeDetectorScript = R"JS((function () {
  'use strict';
  if (window.__prosperobrowserEmeHooked) {
    return;
  }
  try {
    Object.defineProperty(window, '__prosperobrowserEmeHooked', { value: true });
  } catch (e) {
    return;
  }

  function report(keySystem) {
    try {
      var handlers = window.webkit && window.webkit.messageHandlers;
      if (handlers && handlers.prosperobrowser) {
        handlers.prosperobrowser.postMessage({
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

// Reports focus changes of editable elements through the "prosperobrowser" script
// message handler as {type: 'focus', editable}, so the shell can offer text
// entry (EngineViewClient::text_input_focused). Frontends route these
// messages to text_input_focused() instead of script_message().
//
// Right before a {editable: true} report it posts
// {type: 'focus_info', inputType, value}, which frontends pass to
// script_message() unchanged: inputType is the lower-cased <input type>,
// 'textarea' or 'contenteditable'; value is the field's current value, or
// null for passwords, contenteditable and values over 2048 characters (so
// they are neither logged nor pre-filled). The 'focus' report itself never
// carries the value, since frontends log it.
constexpr const char* kFocusReporterScript = R"JS((function () {
  'use strict';
  var TYPES = ['text', 'search', 'email', 'url', 'tel', 'password', 'number'];
  function inputType(e) {
    if (e.isContentEditable) return 'contenteditable';
    if (e.tagName === 'TEXTAREA') return 'textarea';
    return (e.getAttribute('type') || 'text').toLowerCase();
  }
  function editable(e) {
    if (!e) return false;
    if (e.isContentEditable) return true;
    var tag = e.tagName;
    if (tag === 'TEXTAREA') return !e.readOnly && !e.disabled;
    if (tag === 'INPUT') {
      return TYPES.indexOf(inputType(e)) >= 0 && !e.readOnly && !e.disabled;
    }
    return false;
  }
  function send(msg) {
    try {
      window.webkit.messageHandlers.prosperobrowser.postMessage(msg);
    } catch (e) {
    }
  }
  function info(e) {
    var type = inputType(e);
    var value = null;
    if (type !== 'password' && type !== 'contenteditable' && typeof e.value === 'string' &&
        e.value.length <= 2048) {
      value = e.value;
    }
    send({ type: 'focus_info', inputType: type, value: value });
  }
  document.addEventListener('focusin', function (ev) {
    var on = editable(ev.target);
    if (on) info(ev.target);
    send({ type: 'focus', editable: on });
  }, true);
  document.addEventListener('focusout', function () { send({ type: 'focus', editable: false }); }, true);
})();
)JS";

// Maps DualSense input to DOM keyboard events for pages that expect a
// keyboard (e.g. video players). Intentionally empty for now.
constexpr const char* kGamepadKeyBridgeScript = R"JS((function () {
  'use strict';
  // TODO: translate gamepad shortcuts into KeyboardEvents (space, arrows, f).
})();
)JS";

}  // namespace prosperobrowser::core
