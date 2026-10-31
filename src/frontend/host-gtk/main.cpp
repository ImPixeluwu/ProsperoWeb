// SPDX-License-Identifier: GPL-3.0-or-later
// prosperobrowser-host: the browser on a desktop, on top of WebKitGTK 6.0.
#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host_app.h"

namespace {

void usage(const char* argv0) {
  std::printf(
      "Usage: %s [options]\n"
      "  --size WxH            window size (default 1920x1080)\n"
      "  --profile DIR         profile directory (default $XDG_DATA_HOME/prosperobrowser)\n"
      "  --url URL             open URL in the active tab after start\n"
      "  --screenshot PATH     save a PNG of the window and exit\n"
      "  --after-ms N          delay before the screenshot (default 3000)\n"
      "  --demo[=KIND]         chrome demo state over about:blank (KIND: 1, osk, notice, menu)\n"
      "  --lang en|ru          UI language for notices (default from $LANG)\n"
      "  --user-agent UA       override the user agent\n"
      "  --chrome-dir DIR      directory with chrome.html\n"
      "  --input LIST          press gamepad buttons after start, comma separated\n"
      "                        (cross,circle,square,triangle,l1,r1,l2,r2,l3,r3,up,down,\n"
      "                        left,right,options,share,touchpad,wait, or pointer:XxY\n"
      "                        to place the pointer in window coordinates)\n"
      "  --input-delay-ms N    delay before the first --input button (default 1500)\n"
      "  --input-step-ms N     delay between --input buttons (default 250)\n",
      argv0);
}

bool parse_size(const char* s, int& w, int& h) {
  return std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

}  // namespace

int main(int argc, char** argv) {
  prosperobrowser::host::Options opt;
  if (const char* lang = std::getenv("LANG"); lang && std::strncmp(lang, "ru", 2) == 0) {
    opt.language = "ru";
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--size") {
      if (!parse_size(value("--size"), opt.width, opt.height)) {
        std::fprintf(stderr, "--size expects WxH\n");
        return 2;
      }
    } else if (arg == "--profile") {
      opt.profile_dir = value("--profile");
    } else if (arg == "--url") {
      opt.url = value("--url");
    } else if (arg == "--screenshot") {
      opt.screenshot = value("--screenshot");
    } else if (arg == "--after-ms") {
      opt.after_ms = std::atoi(value("--after-ms"));
    } else if (arg == "--demo") {
      opt.demo = "1";
    } else if (arg.rfind("--demo=", 0) == 0) {
      opt.demo = arg.substr(7);
    } else if (arg == "--lang") {
      opt.language = value("--lang");
    } else if (arg == "--user-agent") {
      opt.user_agent = value("--user-agent");
    } else if (arg == "--chrome-dir") {
      opt.chrome_dir = value("--chrome-dir");
    } else if (arg == "--input") {
      std::string list = value("--input");
      std::size_t start = 0;
      while (start <= list.size()) {
        const auto comma = list.find(',', start);
        const auto item = list.substr(start, comma == std::string::npos ? std::string::npos
                                                                        : comma - start);
        if (!item.empty()) opt.input.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    } else if (arg == "--input-delay-ms") {
      opt.input_delay_ms = std::atoi(value("--input-delay-ms"));
    } else if (arg == "--input-step-ms") {
      opt.input_step_ms = std::atoi(value("--input-step-ms"));
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      usage(argv[0]);
      return 2;
    }
  }

  // WebKitGTK keeps native HLS (GStreamer hlsdemux) behind an opt-in; the
  // browser wants it, so enable it unless the user decided otherwise.
  g_setenv("WEBKIT_GST_ENABLE_HLS_SUPPORT", "1", FALSE);

  gtk_init();
  prosperobrowser::host::HostApp app(opt);
  return app.run();
}
