// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/url_normalizer.h"

#include "text_utils.h"

namespace prosperoweb::core {

namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// True for "<scheme>://..." where scheme is RFC 3986 [A-Za-z][A-Za-z0-9+.-]*.
bool has_hierarchical_scheme(std::string_view s) {
  const auto pos = s.find("://");
  if (pos == std::string_view::npos || pos == 0 || !is_alpha(s[0])) {
    return false;
  }
  for (std::size_t i = 1; i < pos; ++i) {
    const char c = s[i];
    if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

bool has_whitespace(std::string_view s) {
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      return true;
    }
  }
  return false;
}

// "host[:port]" part of a scheme-less input.
std::string_view authority_of(std::string_view s) {
  const auto end = s.find_first_of("/?#");
  return end == std::string_view::npos ? s : s.substr(0, end);
}

bool is_port(std::string_view s) {
  if (s.empty() || s.size() > 5) {
    return false;
  }
  for (char c : s) {
    if (!is_digit(c)) {
      return false;
    }
  }
  return true;
}

// Splits "host:port" and validates the optional port.
bool split_host_port(std::string_view authority, std::string_view& host) {
  const auto colon = authority.find(':');
  if (colon == std::string_view::npos) {
    host = authority;
    return true;
  }
  host = authority.substr(0, colon);
  return is_port(authority.substr(colon + 1));
}

bool is_ipv4(std::string_view host) {
  int parts = 0;
  while (true) {
    const auto dot = host.find('.');
    const auto part = host.substr(0, dot);
    if (part.empty() || part.size() > 3) {
      return false;
    }
    int value = 0;
    for (char c : part) {
      if (!is_digit(c)) {
        return false;
      }
      value = value * 10 + (c - '0');
    }
    if (value > 255) {
      return false;
    }
    ++parts;
    if (dot == std::string_view::npos) {
      break;
    }
    host.remove_prefix(dot + 1);
  }
  return parts == 4;
}

bool is_local_host(std::string_view input) {
  std::string_view host;
  if (!split_host_port(authority_of(input), host)) {
    return false;
  }
  return detail::to_lower_ascii(host) == "localhost" || is_ipv4(host);
}

}  // namespace

std::string UrlNormalizer::percent_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (is_alpha(ch) || is_digit(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string UrlNormalizer::normalize(std::string_view input, std::string_view search_template) {
  const std::string_view s = detail::trim(input);
  if (s.empty()) {
    return "about:blank";
  }
  if (detail::starts_with_ci(s, "about:") || detail::starts_with_ci(s, "file:") ||
      has_hierarchical_scheme(s)) {
    return std::string(s);
  }
  if (!has_whitespace(s)) {
    if (is_local_host(s)) {
      return "http://" + std::string(s);
    }
    if (s.find('.') != std::string_view::npos) {
      return "https://" + std::string(s);
    }
  }
  const std::string query = percent_encode(s);
  std::string out(search_template);
  const auto placeholder = out.find("{}");
  if (placeholder == std::string::npos) {
    return out + query;
  }
  out.replace(placeholder, 2, query);
  return out;
}

std::string UrlNormalizer::display_host(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return std::string(url);
  }
  std::string_view rest = url.substr(scheme_end + 3);
  rest = authority_of(rest);
  if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
    rest.remove_prefix(at + 1);
  }
  if (!rest.empty() && rest.front() == '[') {
    const auto close = rest.find(']');
    rest = close == std::string_view::npos ? rest : rest.substr(0, close + 1);
  } else if (const auto colon = rest.find(':'); colon != std::string_view::npos) {
    rest = rest.substr(0, colon);
  }
  if (rest.empty()) {
    return std::string(url);
  }
  return detail::to_lower_ascii(rest);
}

}  // namespace prosperoweb::core
