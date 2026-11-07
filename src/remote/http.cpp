// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/remote/http.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace prosperoweb::remote {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
  const auto it = headers.find(name);
  return it == headers.end() ? std::string() : it->second;
}

bool HttpRequest::keep_alive() const {
  const std::string conn = lower(header("connection"));
  if (version == "HTTP/1.0") {
    return conn == "keep-alive";
  }
  return conn != "close";
}

ParseStatus parse_http_request(std::string_view buffer, std::size_t max_body, HttpRequest& request,
                               std::size_t& consumed) {
  const auto end = buffer.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return buffer.size() > kMaxHeaderBytes ? ParseStatus::TooLarge : ParseStatus::Incomplete;
  }
  if (end > kMaxHeaderBytes) {
    return ParseStatus::TooLarge;
  }
  HttpRequest req;
  std::string_view head = buffer.substr(0, end);
  const auto line_end = head.find("\r\n");
  const std::string_view line = head.substr(0, line_end);
  const auto sp1 = line.find(' ');
  const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
    return ParseStatus::Bad;
  }
  req.method = std::string(line.substr(0, sp1));
  const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  req.version = std::string(line.substr(sp2 + 1));
  if (req.method.empty() || target.empty() || target.front() != '/' ||
      !req.version.starts_with("HTTP/1.")) {
    return ParseStatus::Bad;
  }
  const auto q = target.find('?');
  req.path = std::string(target.substr(0, q));
  if (q != std::string_view::npos) req.query = std::string(target.substr(q + 1));

  std::string_view rest = line_end == std::string_view::npos ? std::string_view{}
                                                            : head.substr(line_end + 2);
  while (!rest.empty()) {
    const auto nl = rest.find("\r\n");
    const std::string_view h = rest.substr(0, nl);
    rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 2);
    const auto colon = h.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return ParseStatus::Bad;
    }
    req.headers[lower(trim(h.substr(0, colon)))] = std::string(trim(h.substr(colon + 1)));
  }
  if (!req.header("transfer-encoding").empty()) {
    return ParseStatus::Bad;  // chunked uploads are not supported
  }
  std::size_t length = 0;
  if (const std::string cl = req.header("content-length"); !cl.empty()) {
    const auto [ptr, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), length);
    if (ec != std::errc() || ptr != cl.data() + cl.size()) {
      return ParseStatus::Bad;
    }
    if (length > max_body) {
      return ParseStatus::TooLarge;
    }
  }
  const std::size_t body_start = end + 4;
  if (buffer.size() - body_start < length) {
    return ParseStatus::Incomplete;
  }
  req.body = std::string(buffer.substr(body_start, length));
  consumed = body_start + length;
  request = std::move(req);
  return ParseStatus::Complete;
}

std::string_view http_reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

HttpResponse HttpResponse::json(int status, const nlohmann::json& body) {
  HttpResponse r;
  r.status = status;
  r.content_type = "application/json";
  r.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  return r;
}

HttpResponse HttpResponse::text(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

std::string HttpResponse::serialize(bool keep_alive) const {
  std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(http_reason(status)) +
                    "\r\n";
  out += "Content-Type: " + content_type + "\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Cache-Control: no-store\r\n";
  out += std::string("Connection: ") + (keep_alive ? "keep-alive" : "close") + "\r\n";
  for (const auto& [name, value] : headers) {
    out += name + ": " + value + "\r\n";
  }
  out += "\r\n";
  out += body;
  return out;
}

}  // namespace prosperoweb::remote
