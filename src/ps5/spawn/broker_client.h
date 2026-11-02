// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client side of the spawn broker (the UI process), plus the minimal HTTP
// client that asks websrv to start the broker. Platform-independent.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_proto.h"

namespace prosperoweb::spawn {

class BrokerClient {
 public:
  // Called on the reader thread for every {"ev":"exited"} notification.
  using ExitCallback = std::function<void(int pid, int status)>;

  // Tries the endpoints in order on every connection attempt.
  BrokerClient(std::vector<BrokerEndpoint> endpoints, ExitCallback on_exit);
  // A single AF_UNIX endpoint.
  BrokerClient(std::string socket_path, ExitCallback on_exit);
  ~BrokerClient();
  BrokerClient(const BrokerClient&) = delete;
  BrokerClient& operator=(const BrokerClient&) = delete;

  // One connection attempt per endpoint, in order. Returns false (errno of
  // the last endpoint) if nobody listens; *failures (may be null) then
  // lists every endpoint with its error.
  bool connect_once(std::string* failures = nullptr);
  // Retries connect_once() until it succeeds or timeout_ms passes.
  bool connect_wait(int timeout_ms, int interval_ms = 100,
                    std::string* failures = nullptr);
  bool connected();
  // The endpoint of the current connection (meaningful while connected):
  // over TCP descriptors cannot be attached, they are named (src_fds).
  BrokerEndpoint endpoint();
  void disconnect();

  // Sends msg (with fds attached) and waits for its reply. Requests are
  // serialised. Returns false if the connection failed or timed out; the
  // client is then disconnected.
  bool request(const nlohmann::json& msg, const int* fds, int nfds,
               nlohmann::json* reply, int timeout_ms);

 private:
  void reader_main(int sock);

  const std::vector<BrokerEndpoint> endpoints_;
  const ExitCallback on_exit_;

  std::mutex request_mu_;  // one request in flight
  std::mutex mu_;          // guards the fields below
  std::condition_variable cv_;
  int sock_ = -1;
  BrokerEndpoint endpoint_;
  bool dead_ = false;  // the reader saw EOF or an error
  std::deque<nlohmann::json> replies_;
  std::thread reader_;
};

// Percent-encodes a query-string value (everything but RFC 3986 unreserved).
std::string url_encode(const std::string& value);

// Escapes a token for websrv's argument splitter (sys.c args_split): a
// backslash before a space or another backslash.
std::string websrv_escape_token(const std::string& token);

// Builds the websrv request that starts the broker as a daemon payload:
//   /hbldr?path=<elf>&cwd=<cwd>&daemon=1&args=<args>&env=<env>
// websrv splits args and env on spaces (backslash escapes a space); args'
// first token becomes the broker's argv[0] and process name.
std::string websrv_broker_target(const std::string& elf, const std::string& cwd,
                                 const std::string& args, const std::string& env);

// The line that asks elfldr (ps5-payload-dev/elfldr at 2a8da002,
// socksrv.c on_connection) to load a payload from the console's own file
// system: "file:<elf>?args=<args>&pipe=0". elfldr reads the URI up to the
// newline, loads <elf> (uri.c uri_get_content), splits the decoded args on
// spaces with a backslash escaping a space (socksrv.c args_split) into
// argv[1..] (argv[0] is the file name), and with pipe=0 does not hand the
// connection to the payload as its stdout/stderr. elfldr passes no
// environment of ours: the payload inherits elfldr's own.
std::string elfldr_payload_line(const std::string& elf, const std::string& args);

// The broker's argument string for elfldr: "--role=spawn-broker" and one
// "--env=NAME=VALUE" per entry of env (each token escaped for args_split),
// which main() exports before anything reads the environment.
std::string elfldr_broker_args(const std::vector<std::string>& env);

// Sends line (plus a newline) to elfldr at host_port ("a.b.c.d:port"),
// half-closes the connection and reads until elfldr closes it. elfldr only
// writes on failure ("[elfldr.elf] Error ..."). Returns 0 when the payload
// was accepted (nothing or no error text came back), 1 when elfldr reported
// an error (*reply holds it), -1 on a connection error (*error).
int elfldr_send(const std::string& host_port, const std::string& line, int timeout_ms,
                std::string* reply, std::string* error);

// Plain HTTP/1.0 GET over TCP. host_port is "a.b.c.d:port" (an IPv4
// literal; the port defaults to 80). Returns the HTTP status code, or -1
// on a connection or protocol error (*error describes it).
int http_get(const std::string& host_port, const std::string& target, int timeout_ms,
             std::string* error);

// Opens and immediately closes a loopback TCP connection to host_port
// ("a.b.c.d:port"). Returns true when a listener accepted it (the port is
// up), false otherwise (*error describes why, e.g. "connect ...: Connection
// refused"). Used to tell "9021 is refused" from "9021 answered" without
// sending a payload, so the happy path (a loader already up) is unchanged.
bool tcp_port_open(const std::string& host_port, int timeout_ms, std::string* error);

// The loaders start_broker tries, in order. loader is PROSPEROWEB_BROKER_LOADER
// ("auto", "elfldr" or "websrv"; anything else is treated as "auto" and noted
// by the caller). native is true in the native title (PROSPEROWEB_TITLE_UI_ROOT
// set): "auto" then tries elfldr first (no websrv needed), websrv first
// otherwise (websrv's tile is already running). A single named loader yields a
// one-element order.
std::vector<std::string> broker_loader_order(const std::string& loader, bool native);

// etaHEN's utility daemon re-declared ABI (LightningMods/etaHEN@dafa13b,
// Source Code/include/msg.hpp): the command socket is AF_UNIX at
// "/system_tmp/etaHEN_util_service", framed as
//   struct { int32 magic=0xDEADBABE; int32 cmd; int32 error; char msg[0x1000]; }
// We re-declare the layout (an interface fact) rather than copy etaHEN's GPLv3
// header. See docs/no-elfldr.md.
constexpr int kEtahenIpcMagic = 0xDEADBABE;
constexpr int kEtahenCmdLaunchElfldr = 0xE1F1D8;  // BREW_UTIL_LAUNCH_ELFLDR
constexpr const char* kEtahenUtilSocketPath = "/system_tmp/etaHEN_util_service";

// Builds the exact bytes of a BREW_UTIL_LAUNCH_ELFLDR request to etaHEN's
// utility daemon: magic, the command, error=0 and a zeroed msg buffer (the
// command carries no payload). The result is sizeof(IPCMessage) bytes.
std::string etahen_launch_elfldr_request();

// Connects the AF_UNIX stream socket at socket_path, sends request, and reads
// one IPCMessage reply. Returns 0 when etaHEN answered with error==0 (it
// launched, or elfldr was already running), 1 when etaHEN answered with a
// non-zero error (*reply_detail may describe it), or -1 when the socket could
// not be reached at all (*error says why; ENOENT means the socket is not in
// this sandbox, the normal case for a uid-1 title). Bounded by timeout_ms.
int etahen_send(const std::string& socket_path, const std::string& request, int timeout_ms,
                std::string* reply_detail, std::string* error);

// Polls probe() until it returns true or total_ms of wall-clock time elapses,
// sleeping interval_ms between attempts (probe is always called at least
// once). Returns the elapsed milliseconds on success, or -1 on timeout. The
// clock and sleep are injectable for tests; the defaults use steady_clock and
// std::this_thread::sleep_for. This is the bounded, no-hang retry discipline
// the update path uses, reused for "ask, then re-poll 9021".
long long poll_until_true(const std::function<bool()>& probe, long long total_ms,
                          long long interval_ms,
                          const std::function<long long()>& clock_ms = {},
                          const std::function<void(long long)>& sleep_ms = {});

// Decides, each time the UI finds no broker connection and may start one,
// whether to launch a fresh broker, keep polling one already in flight, or
// give up. Pure policy, so the connect-race / respawn-loop logic is unit
// tested without real sockets; spawn_glue drives it with the real launch and
// poll-connect. The problem it solves: a broker can take seconds to bind, so a
// single connect attempt races it; and relaunching on every failed spawn spawns
// an endless series of brokers (each then self-exits with "no client"). This
// keeps polling a launched broker for its whole life and caps relaunches.
struct BrokerLaunchPolicy {
  enum Action {
    kLaunch,  // no broker is in flight and the burst is not exhausted: start one
    kPoll,    // a broker was launched recently: keep polling its socket, do not relaunch
    kGiveUp,  // launched max_launches in a row with no connect and still cooling down
  };
  int max_launches = 3;                // brokers to launch in a burst before cooling down
  long long launch_grace_ms = 18000;   // treat a launch as "in flight" this long
  long long cooldown_ms = 30000;       // after a burst, wait this long, then allow one more

  int launches_done = 0;
  long long last_launch_ms = -1;

  // Called when not connected and may_start is true. now_ms is the wall clock.
  // kLaunch/kPoll keep the respawn bounded (no endless rapid series); once the
  // burst is spent it returns kGiveUp until cooldown_ms passes, then kLaunch
  // again — so a loader the user starts later is still picked up, slowly.
  Action decide(long long now_ms) const;
  void on_launched(long long now_ms);  // a loader accepted a broker launch
  void on_connected();                 // the UI connected: clear the counters
};

}  // namespace prosperoweb::spawn
