// SPDX-License-Identifier: GPL-3.0-or-later
//
// libwpe backend loader. libwpe is linked statically, so instead of
// dlopen()ing a backend it asks the embedder through _wpe_loader_interface
// (see webkit-port/README.md, "Link contract"). Returning NULL for every
// object selects libwpe's generic pasteboard and no gamepad provider:
// gamepad input reaches pages through the shell's key bridge instead of the
// Gamepad API. No view/renderer backend is needed on the non-accelerated
// path.
#include <wpe/wpe.h>

namespace {

void* load_object(const char* /*name*/) { return nullptr; }

}  // namespace

extern "C" {
// NOLINTNEXTLINE: the name is libwpe's ABI.
struct wpe_loader_interface _wpe_loader_interface = {load_object, nullptr, nullptr, nullptr,
                                                     nullptr};
}
