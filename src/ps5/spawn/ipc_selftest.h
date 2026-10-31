// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self-test of the IPC primitives WebKit uses between the UI and its
// children, run once when the UI first reaches the spawn broker (always in
// the native title, PROSPEROWEB_IPC_SELFTEST=1 elsewhere), so the log
// answers whether they work across the title's sandbox even if WebKit
// stalls:
//   - an AF_UNIX SOCK_SEQPACKET socketpair made by the UI, one end copied
//     into the broker with rdup (as the broker does for the children);
//   - data both ways over it;
//   - SCM_RIGHTS both ways, carrying an anonymous shared-memory object
//     (shm_open(SHM_ANON)) that the receiver maps and reads.
// And, in each WebProcess/NetworkProcess, a description of the IPC
// descriptor it was started with (fd 3) and whether data waits on it.
#pragma once

#include <nlohmann/json.hpp>

namespace prosperoweb::spawn {

class BrokerClient;

// UI side: runs the test with the connected broker and logs every step.
void ipc_selftest_ui(BrokerClient& client);

// Broker side: the "ipc-selftest" request handler (broker_set_selftest_hook).
nlohmann::json ipc_selftest_broker(const nlohmann::json& request);

// Child side: logs what fd is and, from a background thread, whether
// unread data waits on it after 2, 10 and 30 s.
void ipc_describe_inherited_fd(int fd);

}  // namespace prosperoweb::spawn
