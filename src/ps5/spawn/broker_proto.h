// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wire format of the spawn broker (see README.md, "Spawn broker"): frames
// over an AF_UNIX stream socket, each a 4-byte big-endian payload length
// followed by a UTF-8 JSON object. Descriptors travel as SCM_RIGHTS
// ancillary data attached to the frame that announces them ("nfds").
//
// Platform-independent (POSIX sockets only): compiled for the PS5 and the
// host, where tests/ps5 exercises it.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ps5browser::spawn {

// Upper bound of one frame's JSON payload. A spawn request carries the UI's
// whole environment, which is a few KiB.
inline constexpr size_t kBrokerMaxFrame = 1u << 20;

// Upper bound of descriptors attached to one frame.
inline constexpr int kBrokerMaxFds = 8;

// Default socket path; PS5BROWSER_SPAWN_SOCK overrides it.
inline constexpr const char* kBrokerDefaultSocket = "/data/ps5browser/tmp/spawn.sock";

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

}  // namespace ps5browser::spawn
