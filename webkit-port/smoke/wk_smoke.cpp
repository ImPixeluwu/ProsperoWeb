// SPDX-License-Identifier: GPL-3.0-or-later
// Link smoke test for the static PS5 WebKit: proves the link line in
// webkit-port/README.md resolves the C API. Built, never run.
#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPagePrivatePlayStation.h>
#include <WebKit/WKView.h>
#include <ps5webkit/entry.h>
#include <ps5webkit/spawn.h>
#include <string.h>
#include <wpe/wpe.h>

extern "C" int ps5_spawn_self(const char* const*, const int*, int) { return -1; }
extern "C" int ps5_terminate_process(int) { return -1; }
// libwpe is static: the embedder is its "backend" (pasteboard, gamepads, ...).
extern "C" struct wpe_loader_interface _wpe_loader_interface = { [](const char*) -> void* { return nullptr; }, nullptr, nullptr, nullptr, nullptr };

int main(int argc, char** argv)
{
    if (argc > 0 && !strcmp(argv[0], "WebProcess"))
        return ps5_webkit_web_process_main(argc, argv);
    if (argc > 0 && !strcmp(argv[0], "NetworkProcess"))
        return ps5_webkit_network_process_main(argc, argv);
    WKContextConfigurationRef contextConfiguration = WKContextConfigurationCreate();
    WKContextRef context = WKContextCreateWithConfiguration(contextConfiguration);
    WKPageConfigurationRef pageConfiguration = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(pageConfiguration, context);
    WKViewRef view = WKViewCreate(pageConfiguration);
    static unsigned char pixels[64 * 64 * 4];
    WKPagePaint(WKViewGetPage(view), pixels, WKSizeMake(64, 64), WKRectMake(0, 0, 64, 64));
    return 0;
}
