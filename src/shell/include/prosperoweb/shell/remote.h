// SPDX-License-Identifier: GPL-3.0-or-later
//
// The phone remote control as the shell sees it: switched on and off from
// the menu, it serves a page to phones on the local network that pair with
// a PIN shown on the TV. Implemented by remote::RemoteService (src/remote);
// the frontend also calls its pump() every frame so the phone's commands run
// on the shell's thread (ShellController::remote_command()).
#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace prosperoweb::shell {

class RemoteControl {
 public:
  struct Info {
    std::string url;  // what to open on the phone, e.g. "http://192.168.1.20:17780"
    std::string pin;  // six digits, new every time the remote control starts
  };

  virtual ~RemoteControl() = default;

  // Starts serving the phone page with a fresh PIN. nullopt, with a reason
  // in error, when it cannot listen.
  virtual std::optional<Info> start(std::string& error) = 0;
  // Stops serving and forgets every paired phone.
  virtual void stop() = 0;
  virtual bool running() const = 0;
  // The URL and PIN while running.
  virtual std::optional<Info> info() const = 0;
  // Phones that entered the PIN since start().
  virtual std::size_t paired_devices() const = 0;
  // Forgets the paired phones (they must enter the PIN again).
  virtual void disconnect_all() = 0;
};

}  // namespace prosperoweb::shell
