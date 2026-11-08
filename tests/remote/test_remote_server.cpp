// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "prosperoweb/remote/remote_server.h"
#include "prosperoweb/remote/remote_service.h"

using namespace prosperoweb::remote;
using json = nlohmann::json;

namespace {

int connect_local(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  return fd;
}

void send_all(int fd, const std::string& data) {
  REQUIRE(send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()));
}

// Reads until `count` responses have arrived (counted by status lines) or
// the peer closes; gives up after 3 s.
std::string read_responses(int fd, int count, bool* closed = nullptr) {
  std::string out;
  char buf[4096];
  for (int waited = 0; waited < 3000;) {
    int seen = 0;
    for (std::size_t p = out.find("HTTP/1.1 "); p != std::string::npos;
         p = out.find("HTTP/1.1 ", p + 1)) {
      ++seen;
    }
    if (seen >= count && out.size() >= 4 && out.find("\r\n\r\n") != std::string::npos) {
      // Make sure the last body has arrived: it ends the buffer.
      const auto last = out.rfind("HTTP/1.1 ");
      const auto head_end = out.find("\r\n\r\n", last);
      const auto cl = out.find("Content-Length: ", last);
      if (head_end != std::string::npos && cl != std::string::npos) {
        const auto len = std::stoul(out.substr(cl + 16));
        if (out.size() >= head_end + 4 + len) break;
      }
    }
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) {
      waited += 100;
      continue;
    }
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) {
      if (closed) *closed = true;
      break;
    }
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

Pairing::RandomBytes counter() {
  return [n = std::uint8_t{1}](unsigned char* out, std::size_t size) mutable {
    for (std::size_t i = 0; i < size; ++i) out[i] = n++;
  };
}

}  // namespace

TEST_SUITE("remote.server") {
  TEST_CASE("keep-alive, pipelined requests, bad requests") {
    std::atomic<int> handled{0};
    RemoteServer server(
        [&](const HttpRequest& r) {
          ++handled;
          return HttpResponse::text(200, r.method + " " + r.path + " " + r.body);
        },
        1024);
    std::string error;
    REQUIRE(server.start("127.0.0.1", 0, error));
    CHECK(server.running());
    CHECK(server.port() > 0);

    const int fd = connect_local(server.port());
    send_all(fd,
             "GET /a HTTP/1.1\r\n\r\nPOST /b HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi");
    const std::string two = read_responses(fd, 2);
    CHECK(two.find("GET /a ") != std::string::npos);
    CHECK(two.find("POST /b hi") != std::string::npos);
    // Still open: a third request on the same connection.
    send_all(fd, "GET /c HTTP/1.1\r\nConnection: close\r\n\r\n");
    bool closed = false;
    const std::string third = read_responses(fd, 1, &closed);
    CHECK(third.find("GET /c ") != std::string::npos);
    CHECK(third.find("Connection: close") != std::string::npos);
    close(fd);
    CHECK(handled == 3);

    const int bad = connect_local(server.port());
    send_all(bad, "POST /x HTTP/1.1\r\nContent-Length: 5000\r\n\r\n");
    closed = false;
    const std::string too_large = read_responses(bad, 1, &closed);
    CHECK(too_large.rfind("HTTP/1.1 413", 0) == 0);
    read_responses(bad, 2, &closed);
    CHECK(closed);
    close(bad);
    CHECK(handled == 3);

    server.stop();
    CHECK_FALSE(server.running());
    server.stop();  // idempotent
  }

  TEST_CASE("a taken port is reported") {
    RemoteServer a([](const HttpRequest&) { return HttpResponse::text(200, ""); }, 16);
    std::string error;
    REQUIRE(a.start("127.0.0.1", 0, error));
    RemoteServer b([](const HttpRequest&) { return HttpResponse::text(200, ""); }, 16);
    CHECK_FALSE(b.start("127.0.0.1", a.port(), error));
    CHECK(error.find("cannot listen") != std::string::npos);
    CHECK_FALSE(b.start("not-an-ip", 0, error));
  }

  TEST_CASE("the service pairs a phone over TCP") {
    RemoteService::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.advertised_host = "192.0.2.1";
    RemoteService service(options, counter());
    CHECK_FALSE(service.running());
    CHECK_FALSE(service.info().has_value());
    std::string error;
    const auto info = service.start(error);
    REQUIRE(info.has_value());
    CHECK(info->url == "http://192.0.2.1:" + std::to_string(service.port()));
    CHECK(info->pin.size() == 6);
    CHECK(service.paired_devices() == 0);

    const int fd = connect_local(service.port());
    const std::string body = json{{"pin", info->pin}}.dump();
    send_all(fd, "POST /api/pair HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body);
    const std::string res = read_responses(fd, 1);
    CHECK(res.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(res.find("\"token\"") != std::string::npos);
    close(fd);
    CHECK(service.paired_devices() == 1);

    service.disconnect_all();
    CHECK(service.paired_devices() == 0);

    const std::string old_pin = info->pin;
    service.stop();
    CHECK_FALSE(service.running());
    const auto again = service.start(error);
    REQUIRE(again.has_value());
    CHECK(again->pin != old_pin);
  }
}
