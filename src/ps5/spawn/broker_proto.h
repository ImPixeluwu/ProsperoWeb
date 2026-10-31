// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wire format of the spawn broker (see README.md, "Spawn broker"): frames
// over a stream socket (AF_UNIX, or loopback TCP where the console refuses
// to bind AF_UNIX sockets), each a 4-byte big-endian payload length followed
// by a UTF-8 JSON object. Over AF_UNIX, descriptors travel as SCM_RIGHTS
// ancillary data attached to the frame that announces them ("nfds"); over
// TCP the frame names them instead ("src_pid", "src_fds") and the broker
// copies them out of the sender with rdup.
//
// Platform-independent (POSIX sockets only): compiled for the PS5 and the
// host, where tests/ps5 exercises it.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace prosperoweb::spawn {

// Upper bound of one frame's JSON payload. A spawn request carries the UI's
// whole environment, which is a few KiB.
inline constexpr size_t kBrokerMaxFrame = 1u << 20;

// Upper bound of descriptors attached to one frame.
inline constexpr int kBrokerMaxFds = 8;

// AF_UNIX socket paths tried after $PROSPEROWEB_SPAWN_SOCK, in order.
inline constexpr const char* kBrokerTmpSocket = "/tmp/prosperoweb-spawn.sock";
inline constexpr const char* kBrokerDataSocket = "/data/prosperoweb/tmp/spawn.sock";

// Loopback TCP port of the broker; PROSPEROWEB_SPAWN_PORT overrides it.
inline constexpr int kBrokerDefaultPort = 17771;

// One place the broker listens on / the client connects to.
struct BrokerEndpoint {
  bool tcp = false;
  std::string path;  // AF_UNIX
  int port = 0;      // TCP on 127.0.0.1

  // "unix /tmp/prosperoweb-spawn.sock" or "tcp 127.0.0.1:17771".
  std::string describe() const;
};

// The AF_UNIX paths in the order both sides try them: env_sock (the value
// of PROSPEROWEB_SPAWN_SOCK, may be null or empty), kBrokerTmpSocket,
// kBrokerDataSocket; duplicates dropped.
std::vector<std::string> broker_socket_paths(const char* env_sock);

// The TCP port from env_port (PROSPEROWEB_SPAWN_PORT, may be null);
// kBrokerDefaultPort if unset or not a port number.
int broker_tcp_port(const char* env_port);

// The client's connection order: every AF_UNIX path, then TCP.
std::vector<BrokerEndpoint> broker_endpoints(const char* env_sock, const char* env_port);

// Returns the length-prefixed encoding of msg.
std::string broker_encode_frame(const nlohmann::json& msg);

// Parses a frame payload (without the length prefix). Returns false if it
// is not a JSON object.
bool broker_decode_payload(const std::string& payload, nlohmann::json* msg);

// Sends msg as one frame, attaching fds[0..nfds) to it. Thread-compatible:
// callers that share a socket between threads serialise their sends.
// Returns false on any error (errno is set).
bool broker_send_frame(int sock, const nlohmann::json& msg, const int* fds = nullptr,
                       int nfds = 0);

enum class BrokerRecv { kOk, kClosed, kError };

// Receives one frame. Descriptors that arrive with it are appended to *fds
// (close-on-exec set); on kClosed/kError any received descriptor is closed.
// kClosed means the peer closed the connection between frames.
BrokerRecv broker_recv_frame(int sock, nlohmann::json* msg, std::vector<int>* fds);

// Small accessors that never throw (the JSON library is used without
// exceptions): return the fallback if the key is missing or of another type.
std::string broker_get_string(const nlohmann::json& msg, const char* key,
                              const std::string& fallback = "");
long long broker_get_int(const nlohmann::json& msg, const char* key, long long fallback);
bool broker_get_bool(const nlohmann::json& msg, const char* key, bool fallback);
// Array of strings; false if the key is missing or not an array of strings.
bool broker_get_strings(const nlohmann::json& msg, const char* key,
                        std::vector<std::string>* out);

// "src_fds" of a spawn frame sent over TCP: descriptor numbers in the
// sender's process (named by "src_pid") that the broker copies with rdup.
enum class BrokerSrcFds {
  kAbsent,   // missing or empty: descriptors, if any, came as SCM_RIGHTS
  kPresent,  // *out holds them
  kInvalid,  // not an array of 0..INT_MAX numbers, or too many; *why says
};
BrokerSrcFds broker_get_src_fds(const nlohmann::json& msg, std::vector<int>* out,
                                std::string* why);

}  // namespace prosperoweb::spawn
