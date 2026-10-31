// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small conveniences for the WebKit C API: WKStringRef <-> std::string and a
// scoped WKRelease. We deliberately do not use <WebKit/WKRetainPtr.h> because
// it pulls in WTF headers that are not part of the public C API forwarding
// set, which would couple the frontend compile to the full WebKit tree.
#pragma once

#include <string>
#include <utility>

#include <WebKit/WKString.h>
#include <WebKit/WKType.h>
#include <WebKit/WKURL.h>

namespace prosperoweb::frontend {

// Owns a WKTypeRef and releases it on scope exit.
template <typename T>
class WkOwned {
 public:
  WkOwned() = default;
  explicit WkOwned(T ref) : ref_(ref) {}
  ~WkOwned() {
    if (ref_) {
      WKRelease(ref_);
    }
  }
  WkOwned(const WkOwned&) = delete;
  WkOwned& operator=(const WkOwned&) = delete;
  WkOwned(WkOwned&& other) noexcept : ref_(other.ref_) { other.ref_ = nullptr; }
  WkOwned& operator=(WkOwned&& other) noexcept {
    if (this != &other) {
      if (ref_) {
        WKRelease(ref_);
      }
      ref_ = other.ref_;
      other.ref_ = nullptr;
    }
    return *this;
  }

  T get() const { return ref_; }
  explicit operator bool() const { return ref_ != nullptr; }

 private:
  T ref_ = nullptr;
};

// Creates an adopted (owned) WKStringRef from UTF-8.
inline WkOwned<WKStringRef> make_wk_string(const std::string& s) {
  return WkOwned<WKStringRef>(WKStringCreateWithUTF8CString(s.c_str()));
}

// Copies a WKStringRef into a std::string (UTF-8). Does not release the input.
inline std::string to_std_string(WKStringRef s) {
  if (!s || WKStringIsEmpty(s)) {
    return std::string();
  }
  size_t size = WKStringGetMaximumUTF8CStringSize(s);
  std::string out(size, '\0');
  size_t len = WKStringGetUTF8CString(s, out.data(), size);
  if (len > 0) {
    out.resize(len - 1);  // len includes the NUL terminator
  } else {
    out.clear();
  }
  return out;
}

// Copies a WKURLRef into a std::string. Releases the URL ref (takes ownership).
inline std::string consume_wk_url(WKURLRef url) {
  if (!url) {
    return std::string();
  }
  WKStringRef str = WKURLCopyString(url);
  std::string out = to_std_string(str);
  if (str) {
    WKRelease(str);
  }
  WKRelease(url);
  return out;
}

}  // namespace prosperoweb::frontend
