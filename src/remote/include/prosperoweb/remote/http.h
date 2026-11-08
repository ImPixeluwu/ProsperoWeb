// SPDX-License-Identifier: GPL-3.0-or-later
//
// The small subset of HTTP/1.1 the remote control needs: one request at a
// time per connection, Content-Length bodies (no chunked uploads), and
// responses with a fixed body.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace prosperoweb::remote {

struct HttpRequest {
  std::string method;   // "GET", "POST", ...
  std::string path;     // without the query string
  std::string query;    // after '?', undecoded
  std::string version;  // "HTTP/1.1"
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;

  // The header's value, or "" when absent (name in lower case).
  std::string header(const std::string& name) const;
  // Whether the client wants the connection kept open after the response.
  bool keep_alive() const;
};

enum class ParseStatus { Incomplete, Complete, Bad, TooLarge };

inline constexpr std::size_t kMaxHeaderBytes = 16 * 1024;

// Parses the first request in buffer. On Complete, request is filled and
// consumed is the number of bytes it took (the rest belongs to the next
// request). Bodies larger than max_body give TooLarge.
ParseStatus parse_http_request(std::string_view buffer, std::size_t max_body, HttpRequest& request,
                               std::size_t& consumed);

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers

  static HttpResponse json(int status, const nlohmann::json& body);
  static HttpResponse text(int status, std::string body);

  // The full response; keep_alive picks the Connection header.
  std::string serialize(bool keep_alive) const;
};

// "OK", "Not Found", ...
std::string_view http_reason(int status);

}  // namespace prosperoweb::remote
