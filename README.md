# ps5-browser

A homebrew web browser for the PlayStation 5. It is built on a port of upstream WebKit's PlayStation port to the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), with a small platform-independent core (tabs, history, bookmarks, gamepad input, on-screen keyboard) shared between a host development frontend and the PS5 frontend.

**Status:** work in progress.

## Build

TBD.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The ps5-payload-sdk crt/libc are GPLv3+ and statically linked into the payload, so the browser as a whole is distributed under the same license.

Vendored third-party code keeps its own license, see [third_party/README.md](third_party/README.md).
